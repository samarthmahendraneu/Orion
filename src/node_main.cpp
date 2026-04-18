// node_main.cpp
// Starts a single Orion worker node:
//   1. Registers with the head server via gRPC (Milestone 1)
//   2. Runs a NodeService gRPC server so the head can dispatch tasks (Milestone 2)
//   3. Resilience phase-1: sends periodic heartbeats and re-registers if the
//      head says it has forgotten us.
//
// Usage:  ./node <head_port> <node_port> <node_id>
// Example:./node 50050 6001 node-1
//

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "distributed/node_runtime.h"
#include "distributed/node_service_impl.h"
#include "distributed/functions/function_registry.h"
#include "distributed/functions/builtin_functions.h"
#include "distributed/generated/orion.grpc.pb.h"
#include "distributed/rpc/cas_client.h"
#include "distributed/worker_pool.h"
#include "distributed/observability/logger.h"
#include "distributed/observability/telemetry.h"
#include "distributed/observability/otlp_exporter.h"

static std::atomic<bool> g_running{true};
static std::unique_ptr<grpc::Server> g_grpc_server;


int main(int argc, char* argv[]) {
    std::string head_host = "localhost";
    int         head_port = 50050;
    int         node_port = 6001;
    std::string node_id   = "node-1";

    if (const char* env_head = std::getenv("HEAD_HOST")) head_host = env_head;
    if (argc >= 2) head_port = std::stoi(argv[1]);
    if (argc >= 3) node_port = std::stoi(argv[2]);
    if (argc >= 4) node_id   = argv[3];

    std::string cluster_address = head_host + ":" + std::to_string(head_port);
    std::string node_address    = "127.0.0.1:" + std::to_string(node_port);
    if (const char* env_node = std::getenv("POD_IP")) node_address = std::string(env_node) + ":" + std::to_string(node_port);
    
    std::string listen_address  = "0.0.0.0:" + std::to_string(node_port);

    orion::distributed::NodeRuntime node(
        /*num_workers=*/1,
        node_port,
        cluster_address,
        node_id,
        node_address
    );
    node.start();   // registers with head internally

    // Initialize OTLP Exporter.
    std::string otel_collector = "otel-collector"; // Default k8s service name
    const char* otel_env = std::getenv("OTEL_COLLECTOR_HOST");
    if (otel_env) otel_collector = otel_env;
    orion::observability::OtlpExporter exporter(otel_collector, 4318);

    LOG_INFO("NodeMain", "starting",
             {"node_id", node_id},
             {"node_address", node_address},
             {"otel_collector", otel_collector});

    orion::distributed::FunctionRegistry fn_reg;
    orion::distributed::register_builtin_functions(fn_reg);

    // V2: Initialize CAS Client and Worker Pool
    auto cas_channel = grpc::CreateChannel(cluster_address, grpc::InsecureChannelCredentials());
    auto cas_client = std::make_shared<orion::distributed::CasClient>(cas_channel);
    auto worker_pool = std::make_shared<orion::distributed::WorkerPool>(1); // same as num_workers

    orion::distributed::NodeServiceImpl node_service(node, fn_reg, cas_client, worker_pool);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(listen_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&node_service);

    g_grpc_server = builder.BuildAndStart();
    if (!g_grpc_server) {
        std::cerr << "[Node:" << node_id << "] Failed to start NodeService on "
                  << listen_address << "\n";
        node.stop();
        return 1;
    }
    std::cout << "[Node:" << node_id << "] NodeService listening on "
              << listen_address << "\n" << std::flush;

    std::signal(SIGINT, [](int) {
        g_running = false;
        if (g_grpc_server) g_grpc_server->Shutdown();
    });

    std::cout << "[Node:" << node_id << "] Running. Press Ctrl-C to stop.\n"
              << std::flush;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    g_grpc_server->Shutdown();
    node.stop();
    return 0;
}
