//
// Created by Samarth Mahendra on 12/20/25.
//


// PURPOSE:
// --------
// Implements the Worker execution logic.
//
// WHAT YOU NEED TO IMPLEMENT:
// ----------------------------
// 1. Logic for safely enqueuing tasks.
// 2. Logic for safely dequeuing tasks.
// 3. Logic for executing the task's callable.
// 4. Returning or recording the task result.
//
// THINK ABOUT:
// ------------
// - What happens if the queue is empty?
// - Should execution block or return immediately?
// - Where does the result go? (for now, return it directly)
//
// NOTE:
// -----
// This file should contain ZERO scheduling logic.
// It only executes what it's given.
#include "worker.h"
#include "object_store.h"
#include <functional>
#include <any>
#include <optional>

namespace orion {
    Worker::Worker(ObjectStore& store)
    : store_(store) {}


        void Worker::submit(Task task) {
            ObjectRef ref{task.id};
            {
              // using lock guard for automatic mutex management instead of manual lock/unlock to avoid deadlocks
              std::lock_guard<std::mutex> lock(tasks_mutex);
              // using move to avoid copying the task, don't change

              task_queue.push(std::move(task));
            }
            cv.notify_one(); // Notify one waiting thread that a new task is available
        }

        void Worker::run() {
            std::pair<Task, ObjectRef> item;
            {
                std::unique_lock<std::mutex> lock(tasks_mutex);
                if (task_queue.empty()) {
                    return std::nullopt;
                }
                task = std::move(task_queue.front());
                task_queue.pop();
            }


            // run the task
            std::any result = task.work();
            store_.put(item.second.id, std::move(result));
        }
        std::optional<Task> Worker::peek() {
            std::lock_guard<std::mutex> lock(tasks_mutex);
            if (task_queue.empty()) {
                return std::nullopt;
            }
            return task_queue.front();
        }
}
