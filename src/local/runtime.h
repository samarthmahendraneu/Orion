//
// Created by Samarth Mahendra on 2/27/26.
//

#ifndef RUNTIME_H
#define RUNTIME_H

#pragma once

#include <any>
#include <vector>
#include <memory>

#include "../core/object_store.h"
#include "../core/object_ref.h"
#include "../core/task.h"
#include "../core/worker.h"
#include "../core/scheduler.h"

namespace orion {

    class Runtime {
    public:
        // Create runtime with N worker threads
        explicit Runtime(size_t num_workers, std::string node_id = "local");

        // Submit a task to the system
        ObjectRef submit(Task task);

        // Blocking wait
        void wait(const ObjectRef& ref);

        // Get result (blocking)
        std::any get(const ObjectRef& ref);

        // Access internal store (useful for wiring test contexts)
        ObjectStore& store() { return store_; }

        // Graceful shutdown
        void shutdown();

    private:
        ObjectStore store_;

        std::vector<std::unique_ptr<Worker>> workers_;
        std::unique_ptr<Scheduler> scheduler_;
        std::string node_id_;
    };

} // namespace orion

#endif //RUNTIME_H
