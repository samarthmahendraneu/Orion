#pragma once

#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <random>
#include <condition_variable>
#include <future>

#include "../generated/orion.grpc.pb.h"
#include "cluster_scheduler.h"
#include "../observability/logger.h"

namespace orion::distributed {

enum class RaftRole {
    FOLLOWER,
    CANDIDATE,
    LEADER
};

class RaftConsensus {
public:
    struct Config {
        std::string node_id;
        std::string address;
        std::vector<std::pair<std::string, std::string>> peers; // id -> address
    };

    RaftConsensus(const Config& config, ClusterScheduler& scheduler);
    ~RaftConsensus();

    void start();
    void stop();

    // RPC Handlers
    grpc::Status RequestVote(const VoteRequest* req, VoteReply* reply);
    grpc::Status AppendEntries(const AppendEntriesRequest* req, AppendEntriesReply* reply);
    grpc::Status WhoIsLeader(WhoIsLeaderReply* reply);

    // Client Mutator
    // V2 Optimization: Returns a future to allow batching without blocking the gRPC thread
    std::future<bool> replicate(const OrionLogEntry& entry);

    bool is_leader() const { return role_ == RaftRole::LEADER; }
    int64_t current_term() const { return current_term_.load(); }
    std::string leader_id() const { return leader_id_; }
    std::string leader_address() const;

private:
    void run_election_timeout_();
    void run_heartbeat_loop_();
    void start_election_(std::unique_lock<std::mutex>& lock);
    void become_leader_();
    void become_follower_(int64_t term, const std::string& leader_id);

    // Batching logic
    struct LogEntryBatch {
        OrionLogEntry entry;
        std::promise<bool> promise;
    };
    void run_batch_worker_(std::stop_token st);
    void flush_batch_();

    // Helpers
    std::chrono::milliseconds next_election_timeout_();

private:
    Config config_;
    ClusterScheduler& scheduler_;

    std::atomic<RaftRole> role_{RaftRole::FOLLOWER};
    std::atomic<int64_t> current_term_{0};
    std::string voted_for_;
    std::string leader_id_;

    std::vector<OrionLogEntry> log_;
    int64_t commit_index_{0};
    int64_t last_applied_{0};

    // Leader state
    std::vector<int64_t> next_index_;
    std::vector<int64_t> match_index_;

    std::mutex mu_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    std::chrono::steady_clock::time_point last_heartbeat_;

    std::unique_ptr<std::jthread> election_timer_thread_;
    std::unique_ptr<std::jthread> heartbeat_thread_;
    std::unique_ptr<std::jthread> batch_worker_;

    // Batching state
    std::vector<std::unique_ptr<LogEntryBatch>> pending_batch_;
    static constexpr size_t kMaxBatchSize = 128;
    static constexpr std::chrono::milliseconds kMaxBatchWait = std::chrono::milliseconds(10);

    std::map<std::string, std::unique_ptr<orion::RaftService::Stub>> stubs_;

    mutable std::mt19937 rng_;
};

} // namespace orion::distributed
