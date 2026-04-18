//
// Created by Samarth Mahendra on 2/27/26.
//
// Resilience phase-1 update:
//   - last_seen wallclock per node (set on register + heartbeat)
//   - sweep_expired() evicts nodes whose last_seen > node_ttl
//   - evicted-node-id callback so the scheduler can requeue the victim's
//     in-flight tasks rather than silently losing them
//

#ifndef NODE_REGISTRY_H
#define NODE_REGISTRY_H



#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace orion::distributed {

    struct NodeInfo {
        std::string node_id;
        std::string address;                       // "host:port"
        int available_workers;                     // simple resource metric
        bool alive = true;
        std::chrono::steady_clock::time_point last_seen{};
    };

    class NodeRegistry {
    public:
        // Callback fired exactly once per node-id when sweep_expired() evicts
        // it. Registered by the scheduler so it can requeue in-flight tasks.
        using EvictionCallback = std::function<void(const std::string& /*node_id*/)>;

        // Add or update node (resets last_seen to now()).
        void register_node(const NodeInfo& node);

        // Explicit removal (e.g. graceful node shutdown).
        void remove_node(const std::string& node_id);

        // Node reported in — bump last_seen.
        void heartbeat(const std::string& node_id);

        // Any successful RPC contact with a node is evidence that it is alive,
        // even if the periodic heartbeat stream is temporarily disrupted.
        void note_contact(const std::string& node_id);

        // Returns true if this node-id is known to the registry (alive or not).
        bool has_node(const std::string& node_id) const;

        // List all alive nodes.
        std::vector<NodeInfo> nodes();

        // Round-robin pick over alive nodes only.
        std::optional<NodeInfo> pick_node();

        // Resilience: sweep out nodes whose last_seen is older than `ttl`.
        // Returns the list of evicted node-ids and, as a side effect, fires
        // the eviction callback (if one is registered) for each. We do this
        // with the callback firing OUTSIDE the mutex so the scheduler can
        // take its own locks without risking inversion.
        std::vector<std::string> sweep_expired(std::chrono::seconds ttl);

        // Register the eviction-callback. Only one callback is supported —
        // setting a second one replaces the first. (We don't need a pub-sub
        // fan-out for a single-process head.)
        void set_eviction_callback(EvictionCallback cb);

        void set_node_ttl(std::chrono::seconds ttl) { node_ttl_ = ttl; }
        std::chrono::seconds node_ttl() const { return node_ttl_; }

    private:
        mutable std::mutex mutex_;
        std::unordered_map<std::string, NodeInfo> nodes_;
        size_t rr_index_ = 0;            // round robin pointer

        EvictionCallback eviction_cb_;
        std::chrono::seconds node_ttl_{10};   // tolerate leader failover + heartbeat jitter
    };

} // namespace orion::distributed


#endif //NODE_REGISTRY_H
