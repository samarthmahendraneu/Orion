//
// Created by Samarth Mahendra on 2/27/26.
//

#ifndef CLUSTER_SCHEDULER_H
#define CLUSTER_SCHEDULER_H
#pragma once

#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <mutex>
#include <optional>
#include <string>
#include <chrono>
#include <thread>

#include "../cluster/node_registry.h"
#include "../rpc/node_client.h"

#include "../../core/task.h"
#include "../cluster/global_object_store.h"

namespace orion::distributed {

    // Cluster-level scheduler:
    // - chooses nodes
    // - dispatches tasks
    // - tracks object locations
    // - detects stragglers and launches speculative clones
    // - enforces dependency-wait timeouts so a dead upstream doesn't deadlock
    //   the whole subgraph
    // - verifies SHA-256 hashes between canonical & speculative completers
    //   ("Poisonous Worker" defence)
    struct InFlightTask {
        orion::Task task;
        std::string node_id;
        std::chrono::steady_clock::time_point start_time;
        std::string expected_hash;
        bool is_speculative = false;   // true once a speculative clone has been launched
    };

    class ClusterScheduler : public GlobalObjectStore {
    public:
        ClusterScheduler(NodeRegistry& registry, NodeClient& client);
        ~ClusterScheduler();

        orion::ObjectRef submit(orion::Task task);
        void schedule();

        std::optional<std::any> get_object(const std::string& object_id) override;

        // Updated to handle reported hashes.
        void put_object(const std::string& object_id, std::any value) override;
        void put_object_with_hash(const std::string& object_id, std::any value, const std::string& hash);

        // Straggler detection — monitor in-flight tasks and launch a
        // speculative clone on another node if one is stuck beyond the
        // straggler threshold.
        void check_speculative_execution();

        // NEW (reliability):
        // Scan pending tasks & in-flight tasks for:
        //   - downstreams waiting on a failed upstream (deadletter propagation)
        //   - pending tasks whose deps have never materialised after dep_timeout_
        //   - in-flight tasks that have exceeded in_flight_hard_timeout_
        //     (node crash, lost ReportObjectCreated RPC, etc.)
        // Failed tasks are recorded in failed_objects_ so downstream tasks
        // fail fast rather than waiting forever.
        void check_dependency_timeouts();

        // Returns true if this object has been permanently marked failed.
        bool is_failed(const std::string& object_id) const;

        void start_background_monitoring();
        void stop_background_monitoring();

        // Tunables (exposed for tests).
        void set_straggler_threshold(std::chrono::seconds s) { straggler_threshold_ = s; }
        void set_dep_timeout(std::chrono::seconds s)         { dep_timeout_ = s; }
        void set_in_flight_hard_timeout(std::chrono::seconds s) { in_flight_hard_timeout_ = s; }

    private:
        std::vector<std::pair<std::string, orion::Task>> plan_dispatches_internal_();
        bool deps_ready_internal_(const orion::Task& task) const;
        bool deps_failed_internal_(const orion::Task& task) const;

        // Dispatches `to_dispatch` outside the lock and, on RPC failure,
        // rolls back the in_flight entry and requeues the task.
        void dispatch_and_handle_failures_(
            std::vector<std::pair<std::string, orion::Task>> to_dispatch);

    private:
        NodeRegistry& registry_;
        NodeClient& client_;

        std::unordered_map<std::string, std::any> global_objects_;
        std::queue<orion::Task> pending_;

        // object_id -> InFlightTask
        std::unordered_map<std::string, InFlightTask> in_flight_;

        // object_id -> canonical SHA-256 reported by the first completer.
        // Subsequent (speculative) reports must match this or they are
        // rejected and the reporting node is flagged as suspect.
        std::unordered_map<std::string, std::string> canonical_hashes_;

        // Tasks whose output can never be produced (upstream failed,
        // timeout exceeded, node lost, etc.). Used for deadletter propagation.
        std::unordered_set<std::string> failed_objects_;

        // First-submit wallclock for each pending/in-flight task_id — used
        // by check_dependency_timeouts() to fail tasks that have been
        // waiting on missing deps for too long.
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> submit_time_;

        // Config.
        std::chrono::seconds straggler_threshold_{5};
        std::chrono::seconds dep_timeout_{60};
        std::chrono::seconds in_flight_hard_timeout_{120};

        mutable std::mutex mu_;
        std::unique_ptr<std::jthread> monitor_thread_;
        bool running_ = true;
    };

} // namespace orion::distributed


#endif //CLUSTER_SCHEDULER_H
