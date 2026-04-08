//
// NodeServiceImpl — gRPC NodeService implementation that runs on each worker node.
// Receives ExecuteTask calls from the head, resolves the function via FunctionRegistry,
// and submits the task to the local Runtime.
//

#pragma once

#include <iostream>
#include <string>
#include <cstring>

#include <grpcpp/grpcpp.h>
#include "distributed/generated/orion.grpc.pb.h"

#include "distributed/node_runtime.h"
#include "distributed/functions/function_registry.h"
#include "core/task.h"
#include "core/object_ref.h"

namespace orion::distributed {

class NodeServiceImpl final : public ::orion::NodeService::Service {
public:
    NodeServiceImpl(NodeRuntime& node, FunctionRegistry& fn_reg)
        : node_(node), fn_reg_(fn_reg) {}

    // Called by the head scheduler when it wants this node to run a task.
    grpc::Status ExecuteTask(grpc::ServerContext*,
                             const ::orion::TaskRequest* req,
                             ::orion::TaskReply* reply) override
    {
        std::cout << "[Node:" << node_.node_id()
                  << "] ExecuteTask  task=" << req->task_id()
                  << "  fn=" << req->function_name() << "\n" << std::flush;

        if (!fn_reg_.exists(req->function_name())) {
            std::cerr << "[Node:" << node_.node_id()
                      << "] Unknown function: " << req->function_name() << "\n";
            return grpc::Status(grpc::StatusCode::NOT_FOUND,
                                "Unknown function: " + req->function_name());
        }

        // ── Deserialize literal args from proto (bytes → int or string) ─────────
        std::vector<std::any> literal_args;
        for (const auto& bytes : req->args()) {
            if (req->function_name() == "shell_execute") {
                // For shell_execute, the first argument is always the full command string
                literal_args.push_back(bytes);
            } else if (bytes.size() >= 4) {
                int val = 0;
                std::memcpy(&val, bytes.data(), 4);
                literal_args.push_back(val);
            }
        }

        // Build an orion::Task that the local Runtime can execute.
        // NOTE: We do NOT populate task.deps here. The ClusterHead has already
        // verified that all dependencies are satisfied globally before dispatching.
        // Re-adding them here would cause a deadlock in the local Scheduler since
        // the dependent objects may reside on other nodes.
        orion::Task task;
        task.id            = req->task_id();
        task.function_name = req->function_name();

        // Capture function name and literal args by value.
        // If literal_args is non-empty they are passed directly to the function;
        // otherwise the object-store resolver supplies the dep values (normal path).
        const std::string fn_name = req->function_name();
        const std::string task_id = req->task_id();
        task.work = [this, fn_name, task_id, literal_args](std::vector<std::any> dep_vals) -> std::any {
            // Prefer literal args (sent over the wire) over dep values from store.
            const std::vector<std::any>& effective_args =
                literal_args.empty() ? dep_vals : literal_args;

            std::any result = fn_reg_.invoke(fn_name, effective_args);
            std::cout << "[Node:" << node_.node_id()
                      << "] Task complete  fn=" << fn_name << "\n" << std::flush;

            // Milestone 3: Notify the head that the object is ready
            node_.report_object_created(task_id);
            return result;
        };

        node_.local_runtime().submit(std::move(task));

        reply->set_accepted(true);
        reply->set_node_id(node_.node_id());
        return grpc::Status::OK;
    }

    // Milestone 3 — return the raw bytes of a completed object.
    // For now this is stubbed.
    grpc::Status GetObject(grpc::ServerContext*,
                           const ::orion::ObjectLocationRequest* req,
                           ::orion::ObjectData* reply) override
    {
        std::cout << "[Node:" << node_.node_id()
                  << "] GetObject  object=" << req->object_id()
                  << "  (TODO — Milestone 3)\n" << std::flush;
        reply->set_object_id(req->object_id());
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Milestone 3");
    }

private:
    NodeRuntime&      node_;
    FunctionRegistry& fn_reg_;
};

} // namespace orion::distributed
