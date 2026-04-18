#pragma once

#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <functional>
#include <filesystem>
#include <iostream>

namespace orion::distributed {

namespace fs = std::filesystem;

class WorkerPool {
public:
    explicit WorkerPool(int num_workers, const std::string& base_sandbox_dir = "/tmp/orion-sandbox")
        : base_dir_(base_sandbox_dir) {
        fs::create_directories(base_dir_);
        
        for (int i = 0; i < num_workers; ++i) {
            workers_.emplace_back([this, i] {
                worker_loop(i);
            });
        }
    }

    ~WorkerPool() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& w : workers_) {
            w.join();
        }
    }

    using TaskFunc = std::function<void(const fs::path& /*sandbox_dir*/)>;

    void submit(TaskFunc task) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            tasks_.push(std::move(task));
        }
        cv_.notify_one();
    }

private:
    void worker_loop(int id) {
        fs::path my_sandbox = base_dir_ / ("worker-" + std::to_string(id));
        fs::create_directories(my_sandbox);

        while (true) {
            TaskFunc task;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }

            // Cleanup sandbox before task
            fs::remove_all(my_sandbox);
            fs::create_directories(my_sandbox);

            try {
                task(my_sandbox);
            } catch (const std::exception& e) {
                std::cerr << "[WorkerPool] task failed with exception: " << e.what() << "\n";
            }
        }
    }

    fs::path base_dir_;
    std::vector<std::thread> workers_;
    std::queue<TaskFunc> tasks_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
};

} // namespace orion::distributed
