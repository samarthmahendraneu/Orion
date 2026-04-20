//
// Created by Samarth Mahendra on 2/27/26.
//
// Resilience phase-1: see node_registry.h for the high-level changes.
//

#include "node_registry.h"

#include "../observability/logger.h"
#include "../observability/metrics.h"

namespace orion::distributed {

    void NodeRegistry::register_node(const NodeInfo& node) {
        std::lock_guard<std::mutex> lock(mutex_);
        bool previously_alive = false;
        if (nodes_.count(node.node_id)) {
            previously_alive = nodes_[node.node_id].alive;
        }

        NodeInfo n = node;
        n.last_seen = std::chrono::steady_clock::now();
        n.alive = true;
        nodes_[n.node_id] = n;

        if (!previously_alive) {
            update_alive_indices_internal_();
        }
    }

    void NodeRegistry::remove_node(const std::string& node_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (nodes_.erase(node_id)) {
            update_alive_indices_internal_();
        }
    }

    void NodeRegistry::heartbeat(const std::string& node_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodes_.find(node_id);
        if (it == nodes_.end()) return;              // unknown node — let it re-register
        it->second.alive = true;
        it->second.last_seen = std::chrono::steady_clock::now();
    }

    void NodeRegistry::note_contact(const std::string& node_id) {
        heartbeat(node_id);
    }

    bool NodeRegistry::has_node(const std::string& node_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return nodes_.count(node_id) > 0;
    }

    std::vector<NodeInfo> NodeRegistry::nodes() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<NodeInfo> result;
        result.reserve(alive_indices_.size());

        for (const auto& id : alive_indices_) {
            auto it = nodes_.find(id);
            if (it != nodes_.end()) {
                result.push_back(it->second);
            }
        }
        return result;
    }

    std::optional<NodeInfo> NodeRegistry::pick_node() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (alive_indices_.empty()) return std::nullopt;

        // O(1) Selection
        const std::string& chosen_id = alive_indices_[rr_index_ % alive_indices_.size()];
        rr_index_++;

        auto it = nodes_.find(chosen_id);
        if (it != nodes_.end()) {
            return it->second;
        }

        return std::nullopt;
    }

    std::vector<std::string> NodeRegistry::sweep_expired(std::chrono::seconds ttl) {
        std::vector<std::string> evicted;
        EvictionCallback cb_copy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto now = std::chrono::steady_clock::now();
            bool changed = false;
            for (auto it = nodes_.begin(); it != nodes_.end(); /*++ in body*/) {
                if (it->second.alive && (now - it->second.last_seen) > ttl) {
                    evicted.push_back(it->first);
                    it = nodes_.erase(it);
                    changed = true;
                } else {
                    ++it;
                }
            }
            if (changed) {
                update_alive_indices_internal_();
            }
            cb_copy = eviction_cb_;  // copy under lock; invoke outside
        }

        for (const auto& id : evicted) {
            observability::counters::nodes_evicted().inc();
            LOG_WARN("NodeRegistry", "node_evicted",
                     {"node_id", id},
                     {"ttl_sec", std::to_string(ttl.count())});
            if (cb_copy) cb_copy(id);
        }
        return evicted;
    }

    void NodeRegistry::set_eviction_callback(EvictionCallback cb) {
        std::lock_guard<std::mutex> lock(mutex_);
        eviction_cb_ = std::move(cb);
    }

    void NodeRegistry::update_alive_indices_internal_() {
        // Expects mutex_ to be held
        alive_indices_.clear();
        for (const auto& [id, node] : nodes_) {
            if (node.alive) {
                alive_indices_.push_back(id);
            }
        }
    }

} // namespace orion::distributed
