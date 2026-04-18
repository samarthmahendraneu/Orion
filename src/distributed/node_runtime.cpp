//
// Created by Samarth Mahendra on 2/27/26.
//

#include "node_runtime.h"

#include <iostream>
#include <thread>
#include <chrono>
#include <grpcpp/grpcpp.h>
#include "distributed/generated/orion.grpc.pb.h"

namespace orion::distributed {
    // Simple random ID generator (temporary)
    static std::string generate_node_id() {
        static int counter = 0;
        return "node-" + std::to_string(++counter);
    }

    NodeRuntime::NodeRuntime(size_t num_workers,
                         int port,
                         std::string cluster_address,
                         std::string node_id,
                         std::string address)
    : num_workers_(num_workers),
      port_(port),
      cluster_address_(std::move(cluster_address)),
      running_(false) {
        // If no explicit node_id/address, auto-generate
        if (node_id.empty()) {
            node_id_ = generate_node_id();
        } else {
            node_id_ = std::move(node_id);
        }
        if (address.empty()) {
            address_ = "127.0.0.1:" + std::to_string(port_);
        } else {
            address_ = std::move(address);
        }
    }


    // Start local runtime
    void NodeRuntime::start() {
        if (running_) return;

        std::cout << "[NodeRuntime] Starting node "
                  << node_id_
                  << " on port " << port_ << "\n";

        runtime_ = std::make_unique<orion::Runtime>(num_workers_, node_id_);

        // Initialize gRPC stub for head communication
        if (!cluster_address_.empty()) {
            std::lock_guard<std::mutex> lock(stub_mu_);
            auto channel = grpc::CreateChannel(cluster_address_,
                                               grpc::InsecureChannelCredentials());
            head_stub_ = orion::ClusterHead::NewStub(channel);
        }

        register_with_cluster();

        // Start heartbeat thread
        hb_running_ = true;
        hb_thread_ = std::thread(&NodeRuntime::run_heartbeat_loop, this);

        running_ = true;
    }

    // Stop everything
    void NodeRuntime::stop() {
        if (!running_) return;

        std::cout << "[NodeRuntime] Shutting down node\n";

        hb_running_ = false;
        if (hb_thread_.joinable()) {
            hb_thread_.join();
        }

        if (runtime_) {
            runtime_->shutdown();
            runtime_.reset();
        }

        running_ = false;
    }

    orion::Runtime& NodeRuntime::local_runtime() {
        return *runtime_;
    }

    // Real gRPC registration with the head server.
    void NodeRuntime::register_with_cluster() const {
        int attempt = 0;
        int max_startup_retries = 30; // ~1 minute total with 2s wait
        while (true) {
            attempt++;
            std::cout << "[NodeRuntime] Attempt #" << attempt << " Registering "
                      << node_id_ << " with cluster at " << cluster_address_
                      << "\n" << std::flush;

            orion::RegisterNodeRequest req;
            req.set_node_id(node_id_);
            req.set_address(address_);

            orion::RegisterNodeReply reply;
            grpc::ClientContext ctx;
            ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

            grpc::Status status;
            {
                std::lock_guard<std::mutex> lock(stub_mu_);
                if (!head_stub_) return;
                status = head_stub_->RegisterNode(&ctx, req, &reply);
            }

            if (status.ok()) {
                if (reply.success()) {
                    std::cout << "[NodeRuntime] Registration successful (node=" << node_id_ << ")\n" << std::flush;
                    return;
                } else if (!reply.leader_address().empty()) {
                    std::cout << "[NodeRuntime] Redirecting to leader: " << reply.leader_address() << "\n";
                    cluster_address_ = reply.leader_address();
                    {
                        std::lock_guard<std::mutex> lock(stub_mu_);
                        auto channel = grpc::CreateChannel(cluster_address_, grpc::InsecureChannelCredentials());
                        head_stub_ = orion::ClusterHead::NewStub(channel);
                    }
                    continue;
                }
            }
            
            std::cerr << "[NodeRuntime] Registration attempt " << attempt << " FAILED: "
                      << status.error_message() << " (code=" << status.error_code() << ")\n" << std::flush;
            
            if (attempt >= max_startup_retries) {
                std::cerr << "[NodeRuntime] CRITICAL: Failed to register after " << max_startup_retries << " attempts. Giving up.\n";
                break;
            }
            
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    void NodeRuntime::report_object_created(const std::string& object_id,
                                            const std::string& hash) const {
        // Reliability fix:
        //   Previously the grpc::Status from ReportObjectCreated was discarded.
        //   If the call failed (head restarting, transient network error, TCP
        //   RST after a long pause, etc.), the head would never learn that the
        //   task had completed, and every downstream task waiting on this
        //   object would silently wait forever — the single nastiest
        //   data-loss class in the system.
        //
        //   We now:
        //     1. Set an RPC deadline so a hung head cannot pin this worker thread.
        //     2. Retry up to kMaxAttempts with exponential backoff.
        //     3. Log every failed attempt and a CRITICAL line if we give up
        //        (so the failure is at least loud instead of silent).
        constexpr int kMaxAttempts = 10;
        constexpr int kInitialBackoffMs = 100;
        constexpr int kPerAttemptDeadlineSec = 15;

        auto backoff = std::chrono::milliseconds(kInitialBackoffMs);

        for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
            orion::ObjectReport report;
            report.set_object_id(object_id);
            report.set_node_id(node_id_);
            report.set_hash(hash);

            orion::Empty reply;
            grpc::ClientContext ctx;
            ctx.set_deadline(std::chrono::system_clock::now() +
                             std::chrono::seconds(kPerAttemptDeadlineSec));

            grpc::Status status;
            {
                std::lock_guard<std::mutex> lock(stub_mu_);
                if (!head_stub_) return;
                status = head_stub_->ReportObjectCreated(&ctx, report, &reply);
            }

            if (status.ok()) {
                if (attempt > 1) {
                    std::cout << "[NodeRuntime:" << node_id_
                              << "] ReportObjectCreated succeeded on attempt "
                              << attempt << " object=" << object_id << "\n"
                              << std::flush;
                }
                return;
            }

            if (status.error_code() == grpc::StatusCode::UNAVAILABLE) {
                std::cerr << "[NodeRuntime:" << node_id_
                          << "] ReportObjectCreated hit non-leader. Re-registering...\n" << std::flush;
                register_with_cluster();
            }

            std::cerr << "[NodeRuntime:" << node_id_
                      << "] ReportObjectCreated attempt " << attempt << "/"
                      << kMaxAttempts << " FAILED"
                      << " object=" << object_id
                      << " code=" << status.error_code()
                      << " msg=" << status.error_message() << "\n" << std::flush;

            if (attempt < kMaxAttempts) {
                std::this_thread::sleep_for(backoff);
                backoff *= 2;
            }
        }

        std::cerr << "[NodeRuntime:" << node_id_
                  << "] CRITICAL: ReportObjectCreated giving up after "
                  << kMaxAttempts << " attempts. object=" << object_id
                  << " — head will not learn of this completion. "
                  << "Downstream tasks depending on this object may stall "
                  << "until the dep-timeout sweep reaps them.\n" << std::flush;
    }

    // Sentinel hash value used to signal failure through the existing
    // ReportObjectCreated RPC without any proto schema change.
    static constexpr const char* kTaskFailedSentinel = "__ORION_TASK_FAILED__";

    void NodeRuntime::report_task_failed(const std::string& task_id,
                                         const std::string& reason) const {
        std::cerr << "[NodeRuntime:" << node_id_
                  << "] Reporting FAILURE for task=" << task_id
                  << " reason=" << (reason.empty() ? "exhausted-retries" : reason)
                  << "\n" << std::flush;
        // We reuse report_object_created with the sentinel hash so the head's
        // ReportObjectCreated handler can distinguish failure from success and
        // immediately call on_task_failed_report() rather than waiting for the
        // in_flight_hard_timeout_ sweep.
        report_object_created(task_id, kTaskFailedSentinel);
    }

    void NodeRuntime::run_heartbeat_loop() {
        while (hb_running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            if (!hb_running_.load()) break;

            orion::HeartbeatRequest req;
            req.set_node_id(node_id_);
            req.set_available_workers(num_workers_);

            orion::HeartbeatReply reply;
            grpc::ClientContext ctx;
            ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));

            grpc::Status status;
            {
                std::lock_guard<std::mutex> lock(stub_mu_);
                if (!head_stub_) continue;
                status = head_stub_->Heartbeat(&ctx, req, &reply);
            }

            if (status.ok()) {
                if (!reply.leader_address().empty() && reply.leader_address() != cluster_address_) {
                    std::cout << "[NodeRuntime] Heartbeat redirect to leader: " << reply.leader_address() << "\n";
                    cluster_address_ = reply.leader_address();
                    {
                        std::lock_guard<std::mutex> lock(stub_mu_);
                        auto channel = grpc::CreateChannel(cluster_address_, grpc::InsecureChannelCredentials());
                        head_stub_ = orion::ClusterHead::NewStub(channel);
                    }
                } else if (reply.please_reregister()) {
                    std::cout << "[NodeRuntime] Head asked for re-registration\n";
                    register_with_cluster();
                }
            } else {
                // If the head is gone or we're talking to the wrong peer,
                // force a fresh leader discovery instead of waiting to be
                // evicted from the registry.
                register_with_cluster();
            }
        }
    }

} // namespace orion::distributed
