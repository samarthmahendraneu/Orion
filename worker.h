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
#include <queue>
#include <mutex>
#include <condition_variable>
#include "object_store.h"
#include <functional>
#include <optional>
#include <any>
#include <thread>

namespace orion {

    class Worker {
    public:
        // - Constructor with reference to ObjectStore explicit cuz of single-argument and avoid implicit conversions
        // we don't want implicit conversions cuz it can lead to unexpected behavior and bugs
        // ex : Worker w = someObjectStore; // implicit conversion, not desired now obj = Worker(someObjectStore); // explicit, clear
        explicit Worker(ObjectStore& store);
        ~Worker();
        // - Method to submit a task to this worker.
        ObjectRef submit(Task task);
        // Lifecycle
        void start();
        void stop();


    private:
        void run_loop();   // background thread loop
        void run_one(std::pair<Task, ObjectRef> item);
        // - Task queue
        std::queue<std::pair<Task, ObjectRef>> task_queue;
        // - Synchronization primitives (mutex, condition variable)
        std::mutex tasks_mutex;
        std::condition_variable cv;

        bool running_ = false;
        std::thread worker_thread_;
        ObjectStore& store_;
    };

}


#endif //WORKER_H


