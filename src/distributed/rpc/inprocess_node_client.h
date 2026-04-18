//
// Created by Samarth Mahendra on 2/27/26.
//

#ifndef INPROCESS_NODE_CLIENT_H
#define INPROCESS_NODE_CLIENT_H


#pragma once

#include <unordered_map>
#include <string>
#include <iostream>
#include <exception>

#include "node_client.h"
#include "../node_runtime.h"

namespace orion::distributed {

    class InProcessNodeClient : public NodeClient {
    public:
        void add_node(const std::string& node_id, NodeRuntime* node) {
            nodes_[node_id] = node;
        }

        // Returns true on successful local submission, false if the node is
        // unknown / null or the local runtime throws. We no longer throw here
        // — the ClusterScheduler needs a recoverable failure signal so it can
        // re-queue the task on a different node instead of aborting the
        // cluster process.
        bool submit_task(const std::string& node_id, orion::Task task) override {
            auto it = nodes_.find(node_id);
            if (it == nodes_.end() || it->second == nullptr) {
                std::cerr << "[InProcessNodeClient] ERROR: unknown node_id="
                          << node_id << " task=" << task.id
                          << " — dispatch FAILED\n";
                return false;
            }
            try {
                (void) it->second->local_runtime().submit(std::move(task));
                return true;
            } catch (const std::exception& e) {
                std::cerr << "[InProcessNodeClient] ERROR: submit threw: "
                          << e.what() << "\n";
                return false;
            } catch (...) {
                std::cerr << "[InProcessNodeClient] ERROR: submit threw unknown exception\n";
                return false;
            }
        }

        // Resilience phase-1: in-process tests don't run a NodeService, so
        // there's no remote worker to cancel. We accept the call, log it,
        // and return false (== "no-op, not cancelled"). Tests that exercise
        // speculative-loser logic should use the gRPC client.
        bool cancel_task(const std::string& node_id,
                         const std::string& task_id,
                         const std::string& reason) override {
            std::cerr << "[InProcessNodeClient] cancel_task (noop) node=" << node_id
                      << " task=" << task_id << " reason=" << reason << "\n";
            return false;
        }

    private:
        std::unordered_map<std::string, NodeRuntime*> nodes_;
    };

} // namespace orion::distributed

#endif //INPROCESS_NODE_CLIENT_H
