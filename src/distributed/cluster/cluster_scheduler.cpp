//
// Created by Samarth Mahendra on 2/27/26.
//

#include "cluster_scheduler.h"

#include "cluster_scheduler.h"

namespace orion::distributed {

ClusterScheduler::ClusterScheduler(NodeRegistry& registry, NodeClient& client)
    : registry_(registry), client_(client) {}

orion::ObjectRef ClusterScheduler::submit(orion::Task task) {
    orion::ObjectRef out{task.id};

    {
        std::lock_guard<std::mutex> lock(mu_);
        pending_.push(std::move(task));
    }

    // eager scheduling
    schedule();
    return out;
}

void ClusterScheduler::schedule() {
    std::vector<std::pair<std::string, orion::Task>> to_dispatch;

    {
        std::lock_guard<std::mutex> lock(mu_);
        int q_size = pending_.size();
        for (int i = 0; i < q_size; i++) {
            orion::Task task = std::move(pending_.front());
            pending_.pop();

            // Check dependencies inline to avoid re-locking
            bool ready = true;
            for (const auto& dep : task.deps) {
                if (global_objects_.find(dep.id) == global_objects_.end()) {
                    ready = false;
                    break;
                }
            }

            if (!ready) {
                pending_.push(std::move(task));
                continue;
            }

            // pick a node
            auto node_opt = registry_.pick_node();
            if (!node_opt) {
                pending_.push(std::move(task)); // wait for node
                continue;
            }

            to_dispatch.push_back({node_opt->node_id, std::move(task)});
        }
    }

    // Dispatch target tasks without holding the lock
    for (auto& pair : to_dispatch) {
        client_.submit_task(pair.first, std::move(pair.second));
    }
}

void ClusterScheduler::put_object(const std::string& object_id, std::any value) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        global_objects_[object_id] = std::move(value);
    }
    // Now that a dependency is available globally, try scheduling pending tasks again
    schedule();
}

std::optional<std::any> ClusterScheduler::get_object(const std::string& object_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = global_objects_.find(object_id);
    if (it != global_objects_.end()) {
        return it->second;
    }
    return std::nullopt;
}

bool ClusterScheduler::deps_ready_(const orion::Task& task) const {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& dep : task.deps) {
        if (global_objects_.find(dep.id) == global_objects_.end()) {
            return false;
        }
    }
    return true;
}

} // namespace orion::distributed