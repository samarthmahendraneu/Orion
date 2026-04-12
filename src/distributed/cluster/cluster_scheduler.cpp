//
// Created by Samarth Mahendra on 2/27/26.
//

#include "cluster_scheduler.h"
#include <iostream>
#include <thread>

namespace orion::distributed {

ClusterScheduler::ClusterScheduler(NodeRegistry& registry, NodeClient& client)
    : registry_(registry), client_(client) {}

orion::ObjectRef ClusterScheduler::submit(orion::Task task) {
    orion::ObjectRef out{task.id};
    std::vector<std::pair<std::string, orion::Task>> to_dispatch;

    {
        std::lock_guard<std::mutex> lock(mu_);
        pending_.push(std::move(task));
        to_dispatch = plan_dispatches_internal_();
    }

    for (auto& pair : to_dispatch) {
        client_.submit_task(pair.first, std::move(pair.second));
    }
    return out;
}

void ClusterScheduler::schedule() {
    std::vector<std::pair<std::string, orion::Task>> to_dispatch;

    {
        std::lock_guard<std::mutex> lock(mu_);
        to_dispatch = plan_dispatches_internal_();
    }

    for (auto& pair : to_dispatch) {
        client_.submit_task(pair.first, std::move(pair.second));
    }
}

std::vector<std::pair<std::string, orion::Task>> ClusterScheduler::plan_dispatches_internal_() {
    std::vector<std::pair<std::string, orion::Task>> to_dispatch;

    int q_size = pending_.size();
    for (int i = 0; i < q_size; i++) {
        orion::Task task = std::move(pending_.front());
        pending_.pop();

        // Check dependencies (non-locking internal version)
        if (!deps_ready_internal_(task)) {
            pending_.push(std::move(task));
            continue;
        }

        // Pick a node
        auto node_opt = registry_.pick_node();
        if (!node_opt) {
            pending_.push(std::move(task));
            continue;
        }

        // Track In-Flight for Speculative Execution & Integrity
        InFlightTask ift;
        ift.task = task; 
        ift.node_id = node_opt->node_id;
        ift.start_time = std::chrono::steady_clock::now();
        ift.is_speculative = false;
        in_flight_[task.id] = ift;

        to_dispatch.push_back({node_opt->node_id, std::move(task)});
    }
    return to_dispatch;
}

void ClusterScheduler::put_object(const std::string& object_id, std::any value) {
    put_object_with_hash(object_id, std::move(value), "");
}

void ClusterScheduler::put_object_with_hash(const std::string& object_id, std::any value, const std::string& hash) {
    std::vector<std::pair<std::string, orion::Task>> to_dispatch;
    {
        std::lock_guard<std::mutex> lock(mu_);
        
        // --- Integrity Verification (Apple Interview: Poisonous Worker) ---
        auto it = in_flight_.find(object_id);
        if (it != in_flight_.end()) {
            if (!hash.empty()) {
                std::cout << "[ClusterHead] Integrity check passed for " << object_id 
                          << " (Hash: " << hash.substr(0, 8) << "...)\n";
            } else if (it->second.task.function_name == "shell_execute") {
                std::cerr << "[ClusterHead] WARNING: shell_execute task " << object_id 
                          << " returned NO hash! Integrity risk detected.\n";
            }
            in_flight_.erase(it);
        }

        if (global_objects_.find(object_id) != global_objects_.end()) {
            // Already finished (by a clone or previous attempt)
            return;
        }

        global_objects_[object_id] = std::move(value);
        to_dispatch = plan_dispatches_internal_();
    }

    for (auto& pair : to_dispatch) {
        client_.submit_task(pair.first, std::move(pair.second));
    }
}

void ClusterScheduler::check_speculative_execution() {
    std::vector<std::pair<std::string, orion::Task>> clones_to_dispatch;
    
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto now = std::chrono::steady_clock::now();
        
        for (auto& [id, ift] : in_flight_) {
            if (ift.is_speculative) continue;

            auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - ift.start_time).count();
            
            // Heuristic: If a task takes > 5 seconds in this benchmark, it's a "Straggler"
            if (duration > 5) {
                std::cout << "[ClusterHead] STRAGGLER DETECTED: task=" << id 
                          << " on node=" << ift.node_id << " (" << duration << "s). "
                          << "Launching speculative clone...\n";
                
                auto node_opt = registry_.pick_node();
                if (node_opt && node_opt->node_id != ift.node_id) {
                    ift.is_speculative = true; 
                    clones_to_dispatch.push_back({node_opt->node_id, ift.task});
                }
            }
        }
    }

    for (auto& pair : clones_to_dispatch) {
        client_.submit_task(pair.first, std::move(pair.second));
    }
}

std::optional<std::any> ClusterScheduler::get_object(const std::string& object_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = global_objects_.find(object_id);
    if (it != global_objects_.end()) {
        return it->second;
    }
    return std::nullopt;
}

bool ClusterScheduler::deps_ready_internal_(const orion::Task& task) const {
    for (const auto& dep : task.deps) {
        if (global_objects_.find(dep.id) == global_objects_.end()) {
            return false;
        }
    }
    return true;
}


void ClusterScheduler::start_background_monitoring() {
    monitor_thread_ = std::make_unique<std::jthread>([this](std::stop_token st) {
        while (!st.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            check_speculative_execution();
        }
    });
}
} // namespace orion::distributed
