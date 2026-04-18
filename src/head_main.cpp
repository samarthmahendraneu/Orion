// head_main.cpp — Orion Cluster Head Server
// Implements the gRPC ClusterHead service.
//
// Usage:  ./head [port] [metrics_port]
//   defaults: port=50050, metrics_port=9090
//
// Milestone 1 output:
//   [Head] Listening on 0.0.0.0:50050
//   [Head] RegisterNode  node=node-1  addr=127.0.0.1:6001
//
// Resilience phase-1 additions:
//   - Heartbeat(HeartbeatRequest) returns (HeartbeatReply)
//   - HTTP introspection on metrics_port:  /metrics /cluster /healthz
//   - Background cancel-dispatcher drains ClusterScheduler::take_pending_cancels()
//     and sends CancelTask via the NodeClient.
//

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "distributed/generated/orion.grpc.pb.h"
#include "distributed/cluster/node_registry.h"
#include "distributed/cluster/cluster_scheduler.h"
#include "distributed/rpc/grpc_node_client.h"
#include "distributed/cluster/raft_consensus.h"
#include "distributed/cluster/action_cache.h"
#include "distributed/cluster/cas_store.h"
#include "distributed/cluster/cas_service_impl.h"
#include "distributed/observability/http_server.h"
#include "distributed/observability/metrics.h"
#include "distributed/observability/telemetry.h"
#include "distributed/observability/otlp_exporter.h"

// ── gRPC ClusterHead service implementation ──────────────────────────────────
class HeadServiceImpl final : public orion::ClusterHead::Service {
public:
    HeadServiceImpl(orion::distributed::NodeRegistry&     registry,
                    orion::distributed::ClusterScheduler& scheduler,
                    orion::distributed::RaftConsensus&    raft,
                    orion::distributed::CasStore&         cas,
                    const std::string&                    self_addr)
        : registry_(registry), scheduler_(scheduler), raft_(raft), cas_(cas), self_addr_(self_addr) {}

    grpc::Status RegisterNode(grpc::ServerContext*,
                              const orion::RegisterNodeRequest* req,
                              orion::RegisterNodeReply* reply) override {
        if (!raft_.is_leader()) {
            reply->set_success(false);
            reply->set_leader_address(raft_.leader_address());
            return grpc::Status::OK;
        }
        LOG_INFO("ClusterHead", "register_node",
                 {"node_id", req->node_id()},
                 {"addr", req->address()});
        registry_.register_node({req->node_id(), req->address(),
                                 /*available_workers=*/2, /*alive=*/true,
                                 std::chrono::steady_clock::now()});
        reply->set_success(true);
        return grpc::Status::OK;
    }

    grpc::Status SubmitTask(grpc::ServerContext*,
                            const orion::TaskRequest* req,
                            orion::TaskReply* reply) override {
        orion::observability::counters::tasks_submitted().inc();
        
        if (!raft_.is_leader()) {
            reply->set_accepted(false);
            std::string leader = raft_.leader_address();
            if (!leader.empty() && leader != self_addr_) {
                reply->set_leader_address(leader);
            }
            return grpc::Status::OK;
        }

        // --- Action Cache Lookup ---
        std::vector<std::string> args_vec;
        for (const auto& a : req->args()) args_vec.push_back(a);
        
        std::map<std::string, std::string> dep_actions;
        for (const auto& [id, hash] : req->input_map()) {
            dep_actions[id] = hash;
        }

        std::string action_hash = orion::distributed::ActionCache::compute_action_hash(
            req->function_name(), args_vec, dep_actions);
        
        // --- Tracing instrumentation ---
        std::optional<orion::observability::TraceContext> parent_ctx;
        if (req->has_trace_context()) {
            parent_ctx = orion::observability::TraceContext::from_proto(req->trace_context());
        }

        auto span = orion::observability::Tracer::instance().start_span(
            "SubmitTask", "ClusterHead", 
            parent_ctx ? &(*parent_ctx) : nullptr);
        
        span->set_attribute("task_id", req->task_id());
        span->set_attribute("function", req->function_name());
        orion::observability::g_current_trace_id = span->context().trace_id;
        
        std::string cached_obj_hash = cas_.lookup_action(action_hash);
        if (!cached_obj_hash.empty()) {
            LOG_INFO("ClusterHead", "action_cache_hit",
                     {{"task_id", req->task_id()}, {"action_hash", action_hash}, {"result_hash", cached_obj_hash}});
            
            scheduler_.put_object_with_hash(req->task_id(), std::nullopt, cached_obj_hash);
            
            reply->set_accepted(true);
            reply->set_output_hash(cached_obj_hash);
            
            span->set_attribute("cache_hit", "true");
            span->end();
            orion::observability::g_current_trace_id = "";
            return grpc::Status::OK;
        }

        orion::OrionLogEntry entry;
        entry.set_term(raft_.current_term());
        auto* sub = entry.mutable_submitted();
        sub->mutable_task_req()->CopyFrom(*req);
        
        // Inject tracing context into the log entry.
        span->context().to_proto(sub->mutable_task_req()->mutable_trace_context());
        sub->set_action_hash(action_hash);

        bool replicated = raft_.replicate(entry);
        reply->set_accepted(replicated);
        
        span->set_attribute("replicated", replicated ? "true" : "false");
        span->end();
        orion::observability::g_current_trace_id = "";
        return grpc::Status::OK;
    }

    grpc::Status ReportObjectCreated(grpc::ServerContext*,
                                     const orion::ObjectReport* req,
                                     orion::Empty*) override {

        if (!raft_.is_leader()) {
            // NodeRuntime will catch UNAVAILABLE and re-register to get leader hint
            return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                                "Not leader: redirection required",
                                raft_.leader_address());
        }

        // ── Failure-sentinel path ────────────────────────────────────────────
        // The node sets hash="__ORION_TASK_FAILED__" when shell_execute has
        // exhausted its local retry budget.  We immediately requeue the task
        // on the scheduler rather than waiting for in_flight_hard_timeout_.
        static constexpr const char* kTaskFailedSentinel = "__ORION_TASK_FAILED__";
        if (req->hash() == kTaskFailedSentinel) {
            LOG_ERROR("ClusterHead", "task_failed_sentinel_received",
                      {{"task_id", req->object_id()},
                       {"node_id", req->node_id()}});
            scheduler_.on_task_failed_report(req->object_id());
            return grpc::Status::OK;
        }

        // ── Normal completion path ────────────────────────────────────────────
        orion::observability::counters::tasks_completed().inc();
        registry_.note_contact(req->node_id());
        LOG_INFO("ClusterHead", "report_object_created",
                 {"object_id", req->object_id()},
                 {"hash", req->hash()});

        // --- Action Cache Update ---
        std::string action_hash = scheduler_.pop_action_for_task(req->object_id());
        if (!action_hash.empty()) {
            cas_.link_action(action_hash, req->hash());
        }

        scheduler_.log_performance_metrics(req->object_id());
        scheduler_.put_object_with_hash(req->object_id(), std::nullopt, req->hash());
        return grpc::Status::OK;
    }

    // Resilience phase-1: heartbeat RPC. The NodeRegistry is already
    // responsible for the bookkeeping; we just forward.
    grpc::Status Heartbeat(grpc::ServerContext*,
                           const orion::HeartbeatRequest* req,
                           orion::HeartbeatReply* reply) override {
        orion::observability::counters::heartbeats_received().inc();

        if (!raft_.is_leader()) {
            reply->set_acknowledged(false);
            reply->set_please_reregister(true);
            reply->set_leader_address(raft_.leader_address());
            return grpc::Status::OK;
        }

        const bool known = registry_.has_node(req->node_id());
        if (known) {
            registry_.heartbeat(req->node_id());
            reply->set_acknowledged(true);
            reply->set_please_reregister(false);
        } else {
            // We don't know this node — most likely it was evicted for a TTL
            // miss and is only just waking up. Tell it to re-register.
            reply->set_acknowledged(false);
            reply->set_please_reregister(true);
        }
        return grpc::Status::OK;
    }

    grpc::Status WhoIsLeader(grpc::ServerContext*,
                             const orion::Empty*,
                             orion::WhoIsLeaderReply* reply) override {
        return raft_.WhoIsLeader(reply);
    }

private:
    orion::distributed::NodeRegistry&     registry_;
    orion::distributed::ClusterScheduler& scheduler_;
    orion::distributed::RaftConsensus&    raft_;
    orion::distributed::CasStore&         cas_;
    std::string                           self_addr_;

    std::mutex mu_;
};

// ── gRPC Raft service implementation ─────────────────────────────────────────
class RaftServiceImpl final : public orion::RaftService::Service {
public:
    explicit RaftServiceImpl(orion::distributed::RaftConsensus& raft) : raft_(raft) {}

    grpc::Status RequestVote(grpc::ServerContext*,
                             const orion::VoteRequest* req,
                             orion::VoteReply* reply) override {
        return raft_.RequestVote(req, reply);
    }

    grpc::Status AppendEntries(grpc::ServerContext*,
                               const orion::AppendEntriesRequest* req,
                               orion::AppendEntriesReply* reply) override {
        return raft_.AppendEntries(req, reply);
    }

private:
    orion::distributed::RaftConsensus& raft_;
};

// Build a JSON payload for /cluster that combines counters, the scheduler
// snapshot, and the alive node list. Kept out-of-line so the HTTP route
// closure is a one-liner.
static std::string build_cluster_status_json(
    orion::distributed::NodeRegistry& registry,
    orion::distributed::ClusterScheduler& scheduler,
    orion::distributed::RaftConsensus& raft)
{
    auto snap = scheduler.snapshot();
    auto nodes = registry.nodes();

    std::ostringstream os;
    os << '{';
    os << "\"scheduler\":{"
       << "\"pending\":" << snap.pending
       << ",\"in_flight\":" << snap.in_flight
       << ",\"failed\":" << snap.failed
       << ",\"dead_lettered\":" << snap.dead_lettered
       << ",\"canonical_hashes\":" << snap.canonical_hashes
       << "}";
    os << ",\"leader_id\":\"" << raft.leader_id() << "\"";
    os << ",\"is_leader\":" << (raft.is_leader() ? "true" : "false");
    os << ",\"nodes\":[";
    for (size_t i = 0; i < nodes.size(); i++) {
        if (i) os << ',';
        os << "{\"node_id\":\"" << nodes[i].node_id << "\","
           << "\"address\":\""  << nodes[i].address  << "\","
           << "\"available_workers\":" << nodes[i].available_workers
           << "}";
    }
    os << "]";
    os << ",\"counters\":" << orion::observability::Metrics::instance().render_json();
    os << '}';
    return os.str();
}

// ── main ─────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    std::string port           = (argc > 1) ? argv[1] : "50050";
    int         metrics_port   = (argc > 2) ? std::stoi(argv[2]) : 9090;
    std::string node_id        = (argc > 3) ? argv[3] : ("head-" + port);
    std::string reachable_addr = (argc > 4) ? argv[4] : ("127.0.0.1:" + port);
    std::string server_address = "0.0.0.0:" + port;

    orion::distributed::NodeRegistry registry;
    orion::distributed::GrpcNodeClient grpc_client(registry);
    orion::distributed::ClusterScheduler scheduler(registry, grpc_client);
    
    // Raft configuration
    orion::distributed::RaftConsensus::Config raft_cfg;
    raft_cfg.node_id = node_id;
    raft_cfg.address = reachable_addr;

    // Parse peers: head-2:127.0.0.1:50051 head-3:127.0.0.1:50052
    for (int i = 5; i < argc; ++i) {
        std::string peer_str = argv[i];
        size_t colon = peer_str.find(':');
        if (colon != std::string::npos) {
            raft_cfg.peers.push_back({peer_str.substr(0, colon), peer_str.substr(colon + 1)});
        }
    }
    
    orion::distributed::RaftConsensus raft(raft_cfg, scheduler);
    raft.start();
    scheduler.start_background_monitoring();

    orion::distributed::CasStore cas;
    HeadServiceImpl service(registry, scheduler, raft, cas, raft_cfg.address);
    RaftServiceImpl raft_service(raft);

    orion::distributed::CasServiceImpl cas_service(cas);
    
    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    builder.RegisterService(&raft_service);
    builder.RegisterService(&cas_service);

    auto server = builder.BuildAndStart();
    if (!server) {
        LOG_CRITICAL("ClusterHead", "grpc_bind_failed",
                     {"addr", server_address});
        return 1;
    }
    LOG_INFO("ClusterHead", "grpc_listening", {"addr", server_address});

    // ── Observability HTTP server ────────────────────────────────────────────
    orion::observability::HttpServer http(metrics_port);
    http.route("/metrics", []() {
        return orion::observability::Metrics::instance().render_prometheus();
    });
    http.route("/cluster", [&] { return build_cluster_status_json(registry, scheduler, raft); });
    http.route("/healthz", []() -> std::string { return "ok"; });
    if (!http.start()) {
        LOG_WARN("ClusterHead", "http_bind_failed",
                 {"port", std::to_string(metrics_port)});
    } else {
        // Initialize OTLP Exporter.
    std::string otel_collector = "otel-collector"; // Default k8s service name
    const char* otel_env = std::getenv("OTEL_COLLECTOR_HOST");
    if (otel_env) otel_collector = otel_env;
    orion::observability::OtlpExporter exporter(otel_collector, 4318);

    LOG_INFO("ClusterHead", "starting",
             {"port", port},
             {"metrics_port", std::to_string(metrics_port)},
             {"otel_collector", otel_collector});
    }

    // ── Cancel-dispatcher thread ─────────────────────────────────────────────
    // The scheduler queues speculative-loser cancels; we drain them in a
    // background thread so the scheduler never blocks on RPC.
    std::atomic<bool> running{true};
    std::thread cancel_worker([&] {
        while (running.load()) {
            auto pending = scheduler.take_pending_cancels();
            for (const auto& c : pending) {
                (void) grpc_client.cancel_task(c.node_id, c.task_id, c.reason);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    });

    server->Wait();
    running.store(false);
    cancel_worker.join();
    http.stop();
    return 0;
}
