//
// Created by Samarth Mahendra on 12/20/25.
//

#ifndef WORKER_H
#define WORKER_H

// PURPOSE:
// --------
// Defines a Worker: an execution unit responsible for running Tasks.
//
// Defines a Worker: an execution unit responsible for running Tasks.
//
// A Worker:
// - Receives Tasks
// - Executes them
// - Produces results
//
// NEED TO IMPLEMENT:
// ----------------------------
// 1. An internal task queue.
// 2. A thread-safe way to submit tasks.
// 3. A method to execute a single task (run_once).
// 4. (Later) A loop that runs tasks continuously.
//
// CONSTRAINTS:
// ------------
// - A Worker should NOT make scheduling decisions.
// - A Worker should NOT know about other workers.
// - Assume tasks are already assigned correctly.
#include "task.h"
#include <optional>
#include <queue>
#include <mutex>

namespace orion {

    class Worker {
    public:
        // TODO:
        // - Method to submit a task to this worker.
        void submit(Task task);
        // - Method to peek at the next available task without executing it.
        std::optional<Task> peek();
        // - Method to execute one task if available.
        std::optional<int> run();


    private:
        // TODO:
        // - Task queue
        queue<Task> tasks;
        // - Synchronization primitives (mutex, condition variable)
        std::mutex mutex;
        std::condition_variable condition;
    };

}


#endif //WORKER_H


