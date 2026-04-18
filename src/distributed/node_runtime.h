//
// Created by Samarth Mahendra on 2/27/26.
//

#ifndef NODE_RUNTIME_H
#define NODE_RUNTIME_H


#pragma once

#include <string>
#include <grpcpp/grpcpp.h>
#include "distributed/generated/orion.grpc.pb.h"
#include "../local/runtime.h"

namespace orion::distributed {

    // Represents a single Orion node (one machine)
    // Owns a local runtime and will later host RPC services
    class NodeRuntime {
    public:
        // num_workers = worker threads on this node
        // port = RPC port (used later)
        NodeRuntime(size_t num_workers,
                          int port,
                          std::string cluster_address = "",
                          std::string node_id = "",
                          std::string address = "");


        // Start node (workers + RPC server later)
        void start();

        // Graceful shutdown
        void stop();

        // this will let cluster know hi i am here with x cores
        void register_with_cluster() const;

        // Access local runtime (useful for testing)
        orion::Runtime& local_runtime();

        const std::string& node_id() const { return node_id_; }
        const std::string& address()  const { return address_; }

        // Milestone 3: Notify head that task results are ready
        void report_object_created(const std::string& object_id,
                                   const std::string& hash = "") const;

        // Tell the head that this task failed permanently (exhausted node-side
        // retries).  Uses the "__ORION_TASK_FAILED__" sentinel convention so
        // no proto change is needed.  The head immediately requeues the task
        // on another node subject to its own retry budget.
        void report_task_failed(const std::string& task_id,
                                const std::string& reason = "") const;

    private:
        void run_heartbeat_loop();

        std::unique_ptr<orion::Runtime> runtime_;
        size_t num_workers_;
        int port_;

        // cluster state
        mutable std::string cluster_address_;
        mutable std::unique_ptr<orion::ClusterHead::Stub> head_stub_;

        // identity
        std::string node_id_;
        std::string address_;

        std::atomic<bool> running_{false};
        
        // Background heartbeat
        std::thread hb_thread_;
        std::atomic<bool> hb_running_{false};
        mutable std::mutex stub_mu_;
    };

} // namespace orion::distributed

#endif //NODE_RUNTIME_H
