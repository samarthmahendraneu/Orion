#include "raft_consensus.h"
#include <future>
#include <grpcpp/grpcpp.h>

namespace orion::distributed {

RaftConsensus::RaftConsensus(const Config& config, ClusterScheduler& scheduler)
    : config_(config), scheduler_(scheduler), 
      rng_(static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())) {
    last_heartbeat_ = std::chrono::steady_clock::now();
    
    // Initialize peer stubs
    for (const auto& [id, addr] : config_.peers) {
        auto channel = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
        stubs_[id] = RaftService::NewStub(channel);
    }
}

RaftConsensus::~RaftConsensus() {
    stop();
}

void RaftConsensus::start() {
    running_ = true;
    election_timer_thread_ = std::make_unique<std::jthread>([this](std::stop_token st) {
        while (!st.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            run_election_timeout_();
        }
    });

    heartbeat_thread_ = std::make_unique<std::jthread>([this](std::stop_token st) {
        while (!st.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            run_heartbeat_loop_();
        }
    });
}

void RaftConsensus::stop() {
    running_ = false;
    if (election_timer_thread_) election_timer_thread_->request_stop();
    if (heartbeat_thread_) heartbeat_thread_->request_stop();
}

void RaftConsensus::run_election_timeout_() {
    std::unique_lock<std::mutex> lock(mu_);
    if (role_ == RaftRole::LEADER) return;

    auto now = std::chrono::steady_clock::now();
    if (now - last_heartbeat_ > next_election_timeout_()) {
        start_election_(lock);
    }
}

void RaftConsensus::start_election_(std::unique_lock<std::mutex>& lock) {
    role_ = RaftRole::CANDIDATE;
    current_term_++;
    voted_for_ = config_.node_id;
    last_heartbeat_ = std::chrono::steady_clock::now();

    LOG_INFO("Raft", "election_started", {"term", std::to_string(current_term_)}, {"node", config_.node_id});

    int votes = 1; // Vote for self
    int needed = (config_.peers.size() + 1) / 2 + 1;
    int64_t term = current_term_;
    std::string node_id = config_.node_id;

    lock.unlock(); // DROP LOCK FOR NETWORK CALLS
    for (auto& [id, stub] : stubs_) {
        VoteRequest req;
        req.set_term(term);
        req.set_candidate_id(node_id);
        
        VoteReply reply;
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(500));
        
        grpc::Status status = stub->RequestVote(&ctx, req, &reply);
        if (status.ok() && reply.vote_granted()) {
            votes++;
        } else if (status.ok() && reply.term() > term) {
            lock.lock();
            become_follower_(reply.term(), "");
            return;
        }
    }
    lock.lock(); // RE-ACQUIRE LOCK

    if (role_ == RaftRole::CANDIDATE && current_term_ == term && votes >= needed) {
        become_leader_();
    }
}

void RaftConsensus::become_leader_() {
    role_ = RaftRole::LEADER;
    leader_id_ = config_.node_id;
    LOG_INFO("Raft", "leadership_assumed", {"term", std::to_string(current_term_)});
}

void RaftConsensus::become_follower_(int64_t term, const std::string& leader_id) {
    role_ = RaftRole::FOLLOWER;
    current_term_ = term;
    leader_id_ = leader_id;
    voted_for_ = "";
    last_heartbeat_ = std::chrono::steady_clock::now();
}

void RaftConsensus::run_heartbeat_loop_() {
    std::unique_lock<std::mutex> lock(mu_);
    if (role_ != RaftRole::LEADER) return;

    int64_t term = current_term_;
    std::string node_id = config_.node_id;
    
    lock.unlock(); // DROP LOCK
    for (auto& [id, stub] : stubs_) {
        AppendEntriesRequest req;
        req.set_term(term);
        req.set_leader_id(node_id);
        
        AppendEntriesReply reply;
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(500));
        
        grpc::Status status = stub->AppendEntries(&ctx, req, &reply);
        if (status.ok()) {
            if (reply.term() > term) {
                std::lock_guard<std::mutex> relock(mu_);
                become_follower_(reply.term(), "");
                return;
            }
        }
    }
}

grpc::Status RaftConsensus::RequestVote(const VoteRequest* req, VoteReply* reply) {
    std::lock_guard<std::mutex> lock(mu_);
    if (req->term() < current_term_) {
        reply->set_term(current_term_);
        reply->set_vote_granted(false);
        return grpc::Status::OK;
    }

    if (req->term() > current_term_) {
        become_follower_(req->term(), "");
    }

    if (voted_for_.empty() || voted_for_ == req->candidate_id()) {
        voted_for_ = req->candidate_id();
        last_heartbeat_ = std::chrono::steady_clock::now();
        reply->set_vote_granted(true);
        LOG_INFO("Raft", "vote_granted", {{"candidate", req->candidate_id()}, {"term", std::to_string(req->term())}});
    } else {
        reply->set_vote_granted(false);
        LOG_INFO("Raft", "vote_denied", {{"candidate", req->candidate_id()}, {"reason", "already_voted_for_" + voted_for_}});
    }

    reply->set_term(current_term_);
    return grpc::Status::OK;
}

grpc::Status RaftConsensus::AppendEntries(const AppendEntriesRequest* req, AppendEntriesReply* reply) {
    std::lock_guard<std::mutex> lock(mu_);
    if (req->term() < current_term_) {
        reply->set_term(current_term_);
        reply->set_success(false);
        return grpc::Status::OK;
    }

    last_heartbeat_ = std::chrono::steady_clock::now();
    if (req->term() > current_term_ || role_ != RaftRole::FOLLOWER || leader_id_ != req->leader_id()) {
        LOG_INFO("Raft", "heartbeat_received", {{"leader", req->leader_id()}, {"term", std::to_string(req->term())}});
        become_follower_(req->term(), req->leader_id());
    }

    // Process entries and apply to state machine
    for (const auto& entry : req->entries()) {
        scheduler_.apply_log_entry(entry);
    }

    reply->set_term(current_term_);
    reply->set_success(true);
    return grpc::Status::OK;
}

grpc::Status RaftConsensus::WhoIsLeader(WhoIsLeaderReply* reply) {
    std::lock_guard<std::mutex> lock(mu_);
    reply->set_leader_id(leader_id_);
    reply->set_leader_address(leader_address());
    return grpc::Status::OK;
}

std::string RaftConsensus::leader_address() const {
    if (role_ == RaftRole::LEADER) return config_.address;
    for (const auto& [id, addr] : config_.peers) {
        if (id == leader_id_) return addr;
    }
    return "";
}

std::chrono::milliseconds RaftConsensus::next_election_timeout_() {
    std::uniform_int_distribution<int> dist(500, 1000);
    return std::chrono::milliseconds(dist(rng_));
}

bool RaftConsensus::replicate(const OrionLogEntry& entry) {
    std::unique_lock<std::mutex> lock(mu_);
    if (role_ != RaftRole::LEADER) return false;

    // Fast path: single-node cluster — no peers to replicate to.
    if (stubs_.empty()) {
        scheduler_.apply_log_entry(entry);
        return true;
    }

    int64_t term = current_term_;
    std::string node_id = config_.node_id;

    AppendEntriesRequest req;
    req.set_term(term);
    req.set_leader_id(node_id);
    req.add_entries()->CopyFrom(entry);

    lock.unlock(); // DROP LOCK — RPC calls go out in parallel below

    // Fan-out: send AppendEntries to ALL peers simultaneously.
    // Previously this was a sequential loop — with 2 peers that's 2 × 500ms
    // worst-case, blocking every SubmitTask call.  With parallel fanout it's
    // one round-trip regardless of peer count.
    struct PeerResult { bool success; int64_t term; };
    std::vector<std::future<PeerResult>> peer_futures;
    peer_futures.reserve(stubs_.size());

    for (auto& [id, stub] : stubs_) {
        peer_futures.emplace_back(
            std::async(std::launch::async, [&req, &stub = stub, &id = id, term]() -> PeerResult {
                AppendEntriesReply reply;
                grpc::ClientContext ctx;
                ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(500));
                grpc::Status status = stub->AppendEntries(&ctx, req, &reply);
                if (!status.ok()) {
                    LOG_WARN("Raft", "replicate_rpc_failed",
                             {{"peer", id},
                              {"code",  std::to_string(status.error_code())},
                              {"msg",   status.error_message()}});
                    return {false, 0};
                }
                return {reply.success(), reply.term()};
            })
        );
    }

    int success_count = 1; // count self
    int64_t higher_term = 0;
    for (auto& f : peer_futures) {
        auto [ok, peer_term] = f.get();
        if (ok)                    success_count++;
        if (peer_term > term)      higher_term = std::max(higher_term, peer_term);
    }

    lock.lock(); // RE-ACQUIRE

    if (higher_term > 0) {
        become_follower_(higher_term, "");
        return false;
    }

    int needed = static_cast<int>((stubs_.size() + 1) / 2) + 1;
    if (role_ == RaftRole::LEADER && current_term_ == term && success_count >= needed) {
        scheduler_.apply_log_entry(entry);
        return true;
    }

    return false;
}

} // namespace orion::distributed
