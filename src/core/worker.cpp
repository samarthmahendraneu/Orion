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
#include "context.h"
#include <functional>
#include <any>
#include <iostream>
#include <optional>
#include <thread>

namespace orion {
    Worker::Worker(ObjectStore& store, std::string node_id)
    : store_(store), node_id_(std::move(node_id)) {}


    Worker::~Worker() {
        stop();
    }


        ObjectRef Worker::submit(Task task) {
            ObjectRef ref{task.id};
            {
              // using lock guard for automatic mutex management instead of manual lock/unlock to avoid deadlocks
              std::lock_guard<std::mutex> lock(tasks_mutex);
              // using move to avoid copying the task, don't change

              task_queue.push({std::move(task), ref});
            }
            cv.notify_one(); // Notify one waiting thread that a new task is available
            return ref;
        }
    void Worker::start() {
        {
        std::lock_guard<std::mutex> lock(tasks_mutex);
        running_ = true;
        }
    worker_thread_ = std::thread(&Worker::run_loop, this);
    std::cout << "Starting worker thread..." << std::endl;
    }

    void Worker::stop() {
        {
            std::lock_guard<std::mutex> lock(tasks_mutex);
            if (!running_) return;
            running_ = false;
        }
        cv.notify_all();
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
        std::cout << "Stopping worker thread." << std::endl;
    }

    void Worker::run_loop() {
        orion::set_current_node_id(node_id_);
        
        while (true) {
            std::optional<std::pair<Task, ObjectRef>> item;

            {
                std::unique_lock<std::mutex> lock(tasks_mutex);
                cv.wait(lock, [&] {
                    return !task_queue.empty() || !running_;
                });

                if (!running_ && task_queue.empty()) {
                    return;
                }

                item = std::move(task_queue.front());
                task_queue.pop();
            }

            run_one(std::move(*item));   // ✅ unwrap optional
        }
    }


    void Worker::run_one(std::pair<Task, ObjectRef> item) {

        std::vector<std::any> args;
        args.reserve(item.first.deps.size());

        for (const auto& ref : item.first.deps) {
            args.push_back(store_.get_blocking(ref.id));
        }
        std::any result = item.first.work(std::move(args));


        std::cout << "[Worker] Task result type: " << result.type().name();

        if (result.type() == typeid(int)) {
            std::cout << " value=" << std::any_cast<int>(result);
        }
        else if (result.type() == typeid(double)) {
            std::cout << " value=" << std::any_cast<double>(result);
        }
        else if (result.type() == typeid(std::string)) {
            std::cout << " value=" << std::any_cast<std::string>(result);
        }
        else if (!result.has_value()) {
            std::cout << " <empty>";
        }
        else {
            std::cout << " <unprintable>";
        }

        std::cout << "\n";

        store_.put(item.second.id, std::move(result), node_id_);
    }
}
