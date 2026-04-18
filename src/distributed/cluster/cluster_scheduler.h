//
// Created by Samarth Mahendra on 2/27/26.
//
// Resilience phase-1 additions (on top of the reliability-hardening pass):
//
//   * Per-task retry policy with jittered exponential backoff. A task that
//     fails to dispatch, or sits in-flight on a node that is later evicted,
//     is re-queued up to `max_attempts_` times. Beyond that, the task goes
//     to the dead-letter set (`dead_letter_`) and is treated exactly like a
//     failed dep (downstreams cascade-fail).
//
//   * Dead-node reclamation. The scheduler registers an eviction callback
//     with the NodeRegistry. When the registry reaps a node for missed
//     heartbeats, the scheduler scans `in_flight_` and requeues every task
//     that was pinned to that node — respecting the retry budget.
//
//   * Speculative cancellation hook. When a speculative clone completes
//     first (establishes the canonical hash), we record the LOSING node and
//     task-id so the caller / dispatch layer can send CancelTask. The
//     scheduler itself doesn't know about gRPC; it publishes the list via
//     `take_pending_cancels()` and head_main wires that to GrpcNodeClient.
//
// What we explicitly did NOT do:
//   - No exactly-once semantics — the head is still single-process, so
//     retries can cause duplicate work on the nodes (caller must design
//     tasks to be idempotent, same as before).
//   - No per-task priority yet; retry backoff is purely time-based.
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
#include <random>

#include "../cluster/node_registry.h"
#include "../rpc/node_client.h"

#include "../../core/task.h"
#include "../cluster/global_object_store.h"
#include "../generated/orion.pb.h"

namespace orion::distributed {

    struct InFlightTask {
        orion::Task task;
        std::string node_id;
        std::chrono::steady_clock::time_point start_time;
        std::string expected_hash;
        bool is_speculative = false;   // true once a speculative clone has been launched
        int attempt = 1;               // 1-indexed: 1 == original, 2 == first retry, etc.
    };

    // Published by the scheduler; consumed by whoever holds the NodeClient
    // (head_main wires this to GrpcNodeClient::cancel_task). Kept as a POD
    // so the scheduler doesn't need to depend on any RPC type.
    struct PendingCancel {
        std::string node_id;
        std::string task_id;
        std::string reason;            // e.g. "speculative-loser"
    };

    class ClusterScheduler : public GlobalObjectStore {
    public:
        ClusterScheduler(NodeRegistry& registry, NodeClient& client);
        ~ClusterScheduler();

        orion::ObjectRef submit(orion::Task task);
        void schedule();

        std::optional<std::any> get_object(const std::string& object_id) override;

        void put_object(const std::string& object_id, std::any value) override;
        void put_object_with_hash(const std::string& object_id, std::any value, const std::string& hash);

        void check_speculative_execution();
        void check_dependency_timeouts();

        // --- Raft state-machine integration ----------------------------------
        // Applies a mutation to the local state. Deterministic.
        void apply_log_entry(const orion::OrionLogEntry& entry);

        // Called (typically via NodeRegistry::sweep_expired's callback) when a
        // node is declared dead. Any in-flight tasks pinned to that node are
        // rolled back and re-queued subject to the retry budget.
        void on_node_evicted(const std::string& node_id);

        // Returns true if this object has been permanently marked failed
        // (includes dead-lettered tasks).
        bool is_failed(const std::string& object_id) const;

        // Move any pending cancels out of the scheduler so the caller can
        // issue the real RPC. Thread-safe.
        std::vector<PendingCancel> take_pending_cancels();
        std::string get_canonical_hash(const std::string& object_id) const;

        // Action Mapping lookup
        std::string pop_action_for_task(const std::string& task_id);

        // Performance analysis: logs metrics for a completed task (leader only).
        void log_performance_metrics(const std::string& task_id);

        // --- Observability helpers for /cluster introspection ------------------
        struct Snapshot {
            size_t pending = 0;
            size_t in_flight = 0;
            size_t failed = 0;
            size_t dead_lettered = 0;
            size_t canonical_hashes = 0;
        };
        Snapshot snapshot() const;

        void start_background_monitoring();
        void stop_background_monitoring();

        // Called by the head service when a node explicitly reports a task
        // failure (via the failure-sentinel convention on ReportObjectCreated).
        // Immediately moves the task out of in_flight_ and requeues it subject
        // to the retry budget — so the head doesn't have to wait for
        // in_flight_hard_timeout_ to fire.
        void on_task_failed_report(const std::string& task_id);

        // Tunables (exposed for tests).
        void set_straggler_threshold(std::chrono::seconds s) { straggler_threshold_ = s; }
        void set_dep_timeout(std::chrono::seconds s)         { dep_timeout_ = s; }
        void set_in_flight_hard_timeout(std::chrono::seconds s) { in_flight_hard_timeout_ = s; }
        void set_max_attempts(int n)                         { max_attempts_ = n; }
        void set_retry_base_backoff(std::chrono::milliseconds ms) { retry_base_backoff_ = ms; }

    private:
        std::vector<std::pair<std::string, orion::Task>> plan_dispatches_internal_();
        bool deps_ready_internal_(const orion::Task& task) const;
        bool deps_failed_internal_(const orion::Task& task) const;

        void dispatch_and_handle_failures_(
            std::vector<std::pair<std::string, orion::Task>> to_dispatch);

        // Shared path for "this task just got kicked back to us" (dispatch
        // failure, speculative-loser rollback, or node eviction). Respects
        // `max_attempts_` and moves exhausted tasks to the dead-letter set.
        // MUST be called with mu_ held.
        void requeue_or_deadletter_locked_(orion::Task task, int attempt,
                                           const std::string& reason);

    private:
        NodeRegistry& registry_;
        NodeClient& client_;

        std::unordered_map<std::string, std::any> global_objects_;
        std::queue<orion::Task> pending_;

        // task_id -> attempt number for anything currently pending or in-flight.
        // Needed because the plain `pending_` queue doesn't carry the counter.
        std::unordered_map<std::string, int> attempt_;

        // task_id -> earliest time at which it may be re-dispatched (jittered
        // exponential backoff). Entries are lazily cleared when the planner
        // sees now() >= value.
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> not_before_;

        // Tasks that exhausted the retry budget. Indistinguishable to
        // downstreams from any other permanent failure, but separately
        // counted for observability.
        std::unordered_set<std::string> dead_letter_;

        std::unordered_map<std::string, InFlightTask> in_flight_;

        std::unordered_map<std::string, std::string> canonical_hashes_;
        std::unordered_map<std::string, std::string> task_to_action_;

        std::unordered_set<std::string> failed_objects_;

        std::unordered_map<std::string, std::chrono::steady_clock::time_point> submit_time_;
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> dispatch_time_;
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> completion_time_;

        // Queued-but-not-yet-sent cancels for speculative losers.
        std::vector<PendingCancel> pending_cancels_;

        // Tunables.
        std::chrono::seconds straggler_threshold_{5};
        // dep_timeout_: how long a pending task waits for its dependencies to
        // be satisfied before being declared a failure. For larger projects
        // with deep DAGs, this must be long enough to cover the total height
        // of the dependency tree.
        std::chrono::seconds dep_timeout_{120};
        // in_flight_hard_timeout_: safety net for nodes that crash silently.
        // Keep this comfortably above the normal tail latency for cold builds;
        // explicit node-side failure reports already handle the fast-path.
        std::chrono::seconds in_flight_hard_timeout_{120};
        int max_attempts_{3};
        std::chrono::milliseconds retry_base_backoff_{100};

        // Backoff jitter RNG. Seeded once at construction; guarded by mu_.
        mutable std::mt19937_64 rng_;

        mutable std::mutex mu_;
        std::unique_ptr<std::jthread> monitor_thread_;
        bool running_ = true;
    };

} // namespace orion::distributed


#endif //CLUSTER_SCHEDULER_H
