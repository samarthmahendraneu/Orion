#pragma once
#include <vector>
#include <queue>
#include <mutex>
#include "task.h"
#include "worker.h"
#include "object_store.h"

namespace orion {

    // Minimal dataflow scheduler.
    // - Tracks pending tasks
    // - Dispatches runnable tasks to a worker
    class Scheduler {
    public:
        Scheduler(std::vector<Worker*> workers, ObjectStore& store);

        // Submit a task to the system
        void submit(Task task);

        // Called when a new object is created
        void on_object_created(const ObjectId& id);

        // Try to schedule runnable tasks
        void schedule();

    private:
        bool deps_ready(const Task& task);

        std::vector<Worker*> workers_;
        size_t next_worker_ = 0;
        ObjectStore& store_;

        std::vector<Task> pending_;
        std::queue<Task> ready_;
        std::mutex mutex_;
    };

} // namespace orion