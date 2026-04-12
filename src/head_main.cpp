// head_main.cpp — Orion Cluster Head Server
// Implements the gRPC ClusterHead service.
//
// Usage:  ./head [port]   (default: 50050)
//
// Milestone 1 observable output:
//   [Head] Listening on 0.0.0.0:50050
//   [Head] RegisterNode  node=node-1  addr=127.0.0.1:6001
//   [Head] RegisterNode  node=node-2  addr=127.0.0.1:6002
//
// Milestone 2 observable output (added):
//   [Head] SubmitTask  task=t1  fn=add  → dispatching to node-1
//   [GrpcNodeClient] ExecuteTask(t1) accepted by node-1

#include <iostream>
#include <string>
#include <memory>

#include <grpcpp/grpcpp.h>

#include "distributed/generated/orion.grpc.pb.h"
#include "distributed/cluster/node_registry.h"
#include "distributed/cluster/cluster_scheduler.h"
#include "distributed/rpc/grpc_node_client.h"

// ── gRPC ClusterHead service implementation ──────────────────────────────────
class HeadServiceImpl final : public orion::ClusterHead::Service {
public:
    HeadServiceImpl(orion::distributed::NodeRegistry&     registry,
                    orion::distributed::ClusterScheduler& scheduler)
        : registry_(registry), scheduler_(scheduler) {}

    // ── Milestone 1 ─────────────────────────────────────────────────────────
    grpc::Status RegisterNode(grpc::ServerContext*,
                              const orion::RegisterNodeRequest* req,
                              orion::RegisterNodeReply* reply) override {
        std::cout << "[Head] RegisterNode  node=" << req->node_id()
                  << "  addr=" << req->address() << "\n" << std::flush;

        registry_.register_node({req->node_id(), req->address(),
                                 /*available_workers=*/2, /*alive=*/true});
        reply->set_success(true);
        return grpc::Status::OK;
    }

    // ── Milestone 2 ─────────────────────────────────────────────────────────
    grpc::Status SubmitTask(grpc::ServerContext*,
                            const orion::TaskRequest* req,
                            orion::TaskReply* reply) override {
        std::cout << "[Head] SubmitTask  task=" << req->task_id()
                  << "  fn=" << req->function_name() << "\n" << std::flush;

        // Build an orion::Task from the proto request.
        // The work closure is intentionally empty here — the head only makes the
        // scheduling decision; NodeServiceImpl on the target node does the actual work.
        orion::Task task;
        task.id            = req->task_id();
        task.function_name = req->function_name();

        for (const auto& dep_id : req->dep_ids()) {
            task.deps.push_back(orion::ObjectRef{dep_id});
        }
        // Forward literal args bytes so GrpcNodeClient can include them in the
        // TaskRequest it sends to the worker node.
        for (const auto& bytes : req->args()) {
            task.args.push_back(bytes);
        }

        // No-op work on the head side — real execution happens on the node.
        task.work = [](std::vector<std::any>) -> std::any { return std::any{}; };

        scheduler_.submit(std::move(task));

        // The scheduler picks the node internally; for the reply we report which
        // node was selected (optimistic — from the last cluster pick).
        reply->set_accepted(true);
        // node_id in reply is informational; ClusterScheduler already dispatched
        return grpc::Status::OK;
    }

    // ── Milestone 3 (stubbed) ────────────────────────────────────────────────
    grpc::Status ReportObjectCreated(grpc::ServerContext*,
                                     const orion::ObjectReport* req,
                                     orion::Empty*) override {
        std::cout << "[Head] ReportObjectCreated  object=" << req->object_id()
                  << "  node=" << req->node_id() 
                  << "  hash=" << (req->hash().empty() ? "NONE" : req->hash().substr(0, 8)) << "\n" << std::flush;

        // Milestone 3: Notify the scheduler that the dependency is now ready.
        // We pass the hash for integrity verification (Apple Interview: Poisonous Worker)
        scheduler_.put_object_with_hash(req->object_id(), std::nullopt, req->hash());
        return grpc::Status::OK;
    }

    grpc::Status GetObjectLocation(grpc::ServerContext*,
                                   const orion::ObjectLocationRequest* req,
                                   orion::ObjectLocationReply* reply) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Milestone 3");
    }

private:
    orion::distributed::NodeRegistry&     registry_;
    orion::distributed::ClusterScheduler& scheduler_;
};

// ── main ─────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    std::string port           = (argc > 1) ? argv[1] : "50050";
    std::string server_address = "0.0.0.0:" + port;

    orion::distributed::NodeRegistry registry;

    // Milestone 2: use real gRPC dispatch to worker nodes
    orion::distributed::GrpcNodeClient grpc_client(registry);
    orion::distributed::ClusterScheduler scheduler(registry, grpc_client);
    scheduler.start_background_monitoring();

    HeadServiceImpl service(registry, scheduler);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    auto server = builder.BuildAndStart();
    if (!server) {
        std::cerr << "[Head] Failed to start on " << server_address << "\n";
        return 1;
    }
    std::cout << "[Head] Listening on " << server_address << "\n" << std::flush;
    server->Wait();
    return 0;
}
