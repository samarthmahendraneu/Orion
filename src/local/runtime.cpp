//
// Created by Samarth Mahendra on 2/27/26.
//

#include "runtime.h"

#include "runtime.h"

namespace orion {

    Runtime::Runtime(size_t num_workers, std::string node_id) : node_id_(std::move(node_id)) {

        // Create workers
        for (size_t i = 0; i < num_workers; ++i) {
            workers_.push_back(std::make_unique<Worker>(store_, node_id_));
        }

        // Collect raw pointers for scheduler
        std::vector<Worker*> worker_ptrs;
        for (auto& w : workers_) {
            worker_ptrs.push_back(w.get());
        }

        scheduler_ = std::make_unique<Scheduler>(worker_ptrs, store_);

        // Start workers
        for (auto& w : workers_) {
            w->start();
        }
    }

    ObjectRef Runtime::submit(Task task) {
        scheduler_->submit(task);
        scheduler_->schedule();
        return ObjectRef{task.id};
    }

    void Runtime::wait(const ObjectRef& ref) {
        store_.get_blocking(ref.id);
    }

    std::any Runtime::get(const ObjectRef& ref) {
        return store_.get_blocking(ref.id);
    }

    void Runtime::shutdown() {
        for (auto& w : workers_) {
            w->stop();
        }
    }

}