//
// Created by Samarth Mahendra on 2/27/26.
//

#include "node_runtime.h"

#include <iostream>
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
            auto channel = grpc::CreateChannel(cluster_address_,
                                               grpc::InsecureChannelCredentials());
            head_stub_ = orion::ClusterHead::NewStub(channel);
        }

        register_with_cluster();   // 👈 NEW

        running_ = true;
    }

    // Stop everything
    void NodeRuntime::stop() {
        if (!running_) return;

        std::cout << "[NodeRuntime] Shutting down node\n";

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
        if (!head_stub_) {
            std::cout << "[NodeRuntime] No head stub — skipping gRPC registration\n";
            return;
        }

        std::cout << "[NodeRuntime] Registering "
                  << node_id_
                  << " with cluster at "
                  << cluster_address_
                  << "\n" << std::flush;

        orion::RegisterNodeRequest req;
        req.set_node_id(node_id_);
        req.set_address(address_);

        orion::RegisterNodeReply reply;
        grpc::ClientContext ctx;

        grpc::Status status = head_stub_->RegisterNode(&ctx, req, &reply);

        if (status.ok() && reply.success()) {
            std::cout << "[NodeRuntime] Registration successful (node=" << node_id_ << ")\n" << std::flush;
        } else {
            std::cerr << "[NodeRuntime] Registration FAILED: "
                      << status.error_message() << "\n" << std::flush;
        }
    }

    void NodeRuntime::report_object_created(const std::string& object_id,
                                            const std::string& hash) const {
        if (!head_stub_) return;

        orion::ObjectReport report;
        report.set_object_id(object_id);
        report.set_node_id(node_id_);
        report.set_hash(hash);

        orion::Empty reply;
        grpc::ClientContext ctx;
        head_stub_->ReportObjectCreated(&ctx, report, &reply);
    }

} // namespace orion::distributed