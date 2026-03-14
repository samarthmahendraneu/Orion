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

#include "../cluster/node_registry.h"
#include "../rpc/node_client.h"

#include "../../core/task.h"
#include "../cluster/global_object_store.h"

namespace orion::distributed {

    // Cluster-level scheduler:
    // - chooses nodes
    // - dispatches tasks
    // - tracks object locations
    class ClusterScheduler : public GlobalObjectStore {
    public:
        ClusterScheduler(NodeRegistry& registry, NodeClient& client);

        // Submit a task to the cluster (may or may not dispatch immediately).
        // Returns ObjectRef for the output object (id == task.id).
        orion::ObjectRef submit(orion::Task task);

        // Try to dispatch any runnable tasks.
        void schedule();

        // From GlobalObjectStore interface
        std::optional<std::any> get_object(const std::string& object_id) override;

        // Called when an object is computed on a node
        void put_object(const std::string& object_id, std::any value) override;

    private:
        bool deps_ready_(const orion::Task& task) const;

    private:
        NodeRegistry& registry_;
        NodeClient& client_;

        // object_id -> std::any
        std::unordered_map<std::string, std::any> global_objects_;

        // tasks waiting for deps
        std::queue<orion::Task> pending_;

        mutable std::mutex mu_;
    };

} // namespace orion::distributed


#endif //CLUSTER_SCHEDULER_H
