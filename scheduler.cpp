//
// Created by Samarth Mahendra on 12/22/25.
//
#include "scheduler.h"

namespace orion {

    Scheduler::Scheduler(std::vector<Worker*> workers, ObjectStore& store)
        : workers_(std::move(workers)), store_(store) {
        // Wire automatic notification: when ObjectStore.put() is called,
        // automatically notify scheduler of new objects
        store_.set_on_put_callback([this](const ObjectId& id) {
            this->on_object_created(id);
            this->schedule();
        });
    }

    void Scheduler::submit(Task task) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (deps_ready(task)) {
            ready_.push(std::move(task));
        } else {
            pending_.push_back(std::move(task));
        }
    }

    void Scheduler::on_object_created(const ObjectId&) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = pending_.begin();
        while (it != pending_.end()) {
            if (deps_ready(*it)) {
                ready_.push(std::move(*it));
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void Scheduler::schedule() {
        std::lock_guard<std::mutex> lock(mutex_);

        while (!ready_.empty()) {
            Task task = std::move(ready_.front());
            ready_.pop();
            Worker* w = workers_[next_worker_];
            next_worker_ = (next_worker_ + 1) % workers_.size();
            w->submit(std::move(task));
        }
    }

    bool Scheduler::deps_ready(const Task& task) {
        for (const auto& ref : task.deps) {
            if (!store_.get(ref.id).has_value()) {
                return false;
            }
        }
        return true;
    }

} // namespace orion

