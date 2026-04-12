//
// Created by Samarth Mahendra on 2/27/26.
//

#ifndef CLUSTER_SCHEDULER_H
#define CLUSTER_SCHEDULER_H
#pragma once

#include <unordered_map>
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
    struct InFlightTask {
        orion::Task task;
        std::string node_id;
        std::chrono::steady_clock::time_point start_time;
        std::string expected_hash;
        bool is_speculative = false;
    };

    class ClusterScheduler : public GlobalObjectStore {
    public:
        ClusterScheduler(NodeRegistry& registry, NodeClient& client);

        orion::ObjectRef submit(orion::Task task);
        void schedule();

        std::optional<std::any> get_object(const std::string& object_id) override;
        
        // Updated to handle reported hashes
        void put_object(const std::string& object_id, std::any value) override;
        void put_object_with_hash(const std::string& object_id, std::any value, const std::string& hash);

        // Straggler detection
        void check_speculative_execution();
        void start_background_monitoring();

    private:
        std::vector<std::pair<std::string, orion::Task>> plan_dispatches_internal_();
        bool deps_ready_internal_(const orion::Task& task) const;

    private:
        NodeRegistry& registry_;
        NodeClient& client_;

        std::unordered_map<std::string, std::any> global_objects_;
        std::queue<orion::Task> pending_;
        
        // object_id -> InFlightTask
        std::unordered_map<std::string, InFlightTask> in_flight_;

        mutable std::mutex mu_;
        std::unique_ptr<std::jthread> monitor_thread_;
        bool running_ = true;
    };

} // namespace orion::distributed


#endif //CLUSTER_SCHEDULER_H
