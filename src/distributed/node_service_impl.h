//
// NodeServiceImpl — gRPC NodeService implementation that runs on each worker node.
// Receives ExecuteTask calls from the head, resolves the function via FunctionRegistry,
// and submits the task to the local Runtime.
//
// Resilience phase-1 additions:
//   - CancelTask RPC: marks a task-id as "should-abort". The local runtime's
//     shell_execute wrapper can consult is_cancelled(task_id) to abort any
//     long-running subprocess. For pure-compute functions, the flag is checked
//     before invoking the function (cheap) — fine-grained mid-task preemption
//     would need cooperative probing inside user functions and is deliberately
//     not in this phase.
//

#pragma once

#include <atomic>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_set>

#include <grpcpp/grpcpp.h>
#include "distributed/generated/orion.grpc.pb.h"
#include "distributed/rpc/cas_client.h"
#include "distributed/worker_pool.h"
#include "distributed/observability/logger.h"
#include "distributed/observability/telemetry.h"

#include "distributed/node_runtime.h"
#include "distributed/functions/function_registry.h"
#include "core/task.h"
#include "core/object_ref.h"
#include "distributed/functions/hash_util.h"

namespace orion::distributed {

class NodeServiceImpl final : public ::orion::NodeService::Service {
public:
    NodeServiceImpl(NodeRuntime& node, FunctionRegistry& fn_reg,
                    std::shared_ptr<CasClient> cas_client,
                    std::shared_ptr<WorkerPool> worker_pool)
        : node_(node), fn_reg_(fn_reg), cas_client_(cas_client), worker_pool_(worker_pool) {}

    // V2: Content-addressed execution with sandboxing.
    grpc::Status ExecuteTask(grpc::ServerContext*,
                             const ::orion::TaskRequest* req,
                             ::orion::TaskReply* reply) override
    {
        const std::string task_id = req->task_id();
        const std::string fn_name = req->function_name();
        
        LOG_INFO("NodeService", "execute_task_v2", {{"task_id", task_id}, {"fn", fn_name}});
        
        // Register for cancellation
        mark_running_(task_id);

        // Capture V2 input map and other metadata
        struct TaskContext {
            std::string id;
            std::string function;
            std::map<std::string, std::string> inputs;
            std::string command;
            observability::TraceContext trace_ctx;
        };

        auto ctx = std::make_shared<TaskContext>();
        ctx->id = task_id;
        ctx->function = fn_name;
        for (auto const& [name, hash] : req->input_map()) {
            ctx->inputs[name] = hash;
        }

        if (fn_name == "shell_execute" && !req->args().empty()) {
            ctx->command = std::string(req->args(0).begin(), req->args(0).end());
        }
        
        if (req->has_trace_context()) {
            ctx->trace_ctx = observability::TraceContext::from_proto(req->trace_context());
        }

        // Submit to worker pool
        worker_pool_->submit([this, ctx](const fs::path& sandbox_dir) {
            if (consume_cancel_(ctx->id)) {
                LOG_INFO("NodeService", "task_cancelled_skip", {{"task_id", ctx->id}});
                return;
            }

            // --- Tracing instrumentation ---
            auto span = observability::Tracer::instance().start_span(
                "ExecuteTask", "NodeWorker", 
                ctx->trace_ctx.is_valid() ? &ctx->trace_ctx : nullptr);
            span->set_attribute("task_id", ctx->id);
            span->set_attribute("function", ctx->function);
            if (!ctx->command.empty()) span->set_attribute("command", ctx->command);
            
            // Set thread-local trace ID for the worker thread.
            observability::g_current_trace_id = span->context().trace_id;

            // 1. FETCH DEPENDENCIES
            for (auto const& [name, hash] : ctx->inputs) {
                fs::path dest = sandbox_dir / name;
                // Ensure parent directory exists for files in subdirs
                if (dest.has_parent_path()) {
                    fs::create_directories(dest.parent_path());
                }

                if (!cas_client_->fetch_blob(hash, dest)) {
                    LOG_ERROR("NodeService", "fetch_failed", {{"task_id", ctx->id}, {"file", name}});
                    node_.report_task_failed(ctx->id, "dependency-fetch-failed");
                    return;
                }
            }

            // 2. EXECUTE
            bool success = false;
            std::string output_filename;
            
            // Build arguments for the registered function
            std::vector<std::any> fn_args;
            if (ctx->function == "shell_execute") {
                fn_args.push_back(ctx->command);
                fn_args.push_back(sandbox_dir.string());
                
                // Heuristic for output filename detection stays here for task reporting
                output_filename = ctx->id;
                size_t o_pos = ctx->command.find("-o ");
                if (o_pos != std::string::npos) {
                    std::stringstream ss(ctx->command.substr(o_pos + 3));
                    ss >> output_filename;
                }
            } else {
                // For other functions, maybe parse req->args() if needed
                // Currently non-shell functions are placeholders
            }

            if (fn_reg_.exists(ctx->function)) {
                try {
                    std::any res = fn_reg_.invoke(ctx->function, fn_args);
                    if (res.type() == typeid(int)) {
                        success = (std::any_cast<int>(res) != 0);
                    } else if (res.type() == typeid(bool)) {
                        success = std::any_cast<bool>(res);
                    } else {
                        success = true; // Assume success if function returned something else
                    }
                } catch (const std::exception& e) {
                    LOG_ERROR("NodeService", "function_invocation_failed", {{"task_id", ctx->id}, {"error", e.what()}});
                    success = false;
                }
            } else {
                LOG_ERROR("NodeService", "function_not_found", {{"task_id", ctx->id}, {"fn", ctx->function}});
                success = false;
            }

            // 3. UPLOAD RESULT & REPORT
            if (success) {
                std::string result_hash = "";
                if (!output_filename.empty()) {
                    fs::path out_path = sandbox_dir / output_filename;
                    if (fs::exists(out_path)) {
                        result_hash = cas_client_->upload_blob(out_path);
                    }
                }
                
                node_.report_object_created(ctx->id, result_hash);
                LOG_INFO("NodeService", "task_success", {{"task_id", ctx->id}, {"hash", result_hash}});
            } else {
                node_.report_task_failed(ctx->id, "execution-failed");
                LOG_ERROR("NodeService", "task_failed", {{"task_id", ctx->id}});
            }

            span->set_attribute("success", success ? "true" : "false");
            span->end();
            observability::g_current_trace_id = "";
            clear_running_(ctx->id);
        });

        reply->set_accepted(true);
        reply->set_node_id(node_.node_id());
        return grpc::Status::OK;
    }

    // Resilience phase-1: honor head cancellation requests.
    grpc::Status CancelTask(grpc::ServerContext*,
                            const ::orion::CancelRequest* req,
                            ::orion::CancelReply* reply) override
    {
        std::cout << "[Node:" << node_.node_id()
                  << "] CancelTask  task=" << req->task_id()
                  << "  reason=" << req->reason() << "\n" << std::flush;
        const bool ok = request_cancel_(req->task_id());
        reply->set_cancelled(ok);
        return grpc::Status::OK;
    }

    // Milestone 3 — return the raw bytes of a completed object.
    grpc::Status GetObject(grpc::ServerContext*,
                           const ::orion::ObjectLocationRequest* req,
                           ::orion::ObjectData* reply) override
    {
        std::cout << "[Node:" << node_.node_id()
                  << "] GetObject  object=" << req->object_id() << "\n" << std::flush;

        auto obj_opt = node_.local_runtime().store().get(req->object_id());
        if (!obj_opt) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "Object not found: " + req->object_id());
        }

        reply->set_object_id(req->object_id());
        // For simple POD types or strings, we can serialize to bytes.
        // For this POC, we'll assume the object is already a string or bytes.
        if (obj_opt->type() == typeid(std::string)) {
            reply->set_data(std::any_cast<std::string>(*obj_opt));
        }
        
        return grpc::Status::OK;
    }

    // Exposed so the runtime's work-closure can probe for cancellation if
    // the user function chooses to cooperate. Not used by the default builtins.
    bool is_cancelled(const std::string& task_id) const {
        std::lock_guard<std::mutex> lock(cancel_mu_);
        return cancelled_.count(task_id) > 0;
    }

private:
    void mark_running_(const std::string& task_id) {
        std::lock_guard<std::mutex> lock(cancel_mu_);
        running_.insert(task_id);
    }
    void clear_running_(const std::string& task_id) {
        std::lock_guard<std::mutex> lock(cancel_mu_);
        running_.erase(task_id);
        cancelled_.erase(task_id);
    }
    // Returns true if the task was known to us (either still running or
    // already cancelled) — for RPC replies.
    bool request_cancel_(const std::string& task_id) {
        std::lock_guard<std::mutex> lock(cancel_mu_);
        if (running_.count(task_id) == 0) {
            // Already done (or never seen). Nothing to do.
            return false;
        }
        cancelled_.insert(task_id);
        return true;
    }
    // Used by the work closure to atomically check + reset the cancel flag.
    bool consume_cancel_(const std::string& task_id) {
        std::lock_guard<std::mutex> lock(cancel_mu_);
        if (cancelled_.erase(task_id)) {
            running_.erase(task_id);
            return true;
        }
        return false;
    }

    NodeRuntime&      node_;
    FunctionRegistry& fn_reg_;
    std::shared_ptr<CasClient> cas_client_;
    std::shared_ptr<WorkerPool> worker_pool_;

    mutable std::mutex cancel_mu_;
    std::unordered_set<std::string> running_;
    std::unordered_set<std::string> cancelled_;
};

} // namespace orion::distributed
