#pragma once

#include <string>
#include <vector>
#include <deque>
#include <map>
#include <mutex>
#include <algorithm>
#include <chrono>

namespace orion::distributed {

class LatencyTracker {
public:
    struct FunctionStats {
        std::deque<int64_t> samples;
        size_t max_samples = 100;
        int64_t median_ms = 0;

        void add_sample(int64_t ms) {
            samples.push_back(ms);
            if (samples.size() > max_samples) {
                samples.pop_front();
            }
            // Update median (simplified: sort copy and pick middle)
            std::vector<int64_t> sorted(samples.begin(), samples.end());
            std::sort(sorted.begin(), sorted.end());
            if (!sorted.empty()) {
                median_ms = sorted[sorted.size() / 2];
            }
        }
    };

    void record_completion(const std::string& function_name, int64_t duration_ms) {
        std::lock_guard<std::mutex> lock(mu_);
        stats_[function_name].add_sample(duration_ms);
    }

    int64_t get_straggler_threshold_ms(const std::string& function_name, int64_t default_threshold_ms) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = stats_.find(function_name);
        if (it == stats_.end() || it->second.samples.size() < 5) {
            return default_threshold_ms;
        }

        // Adaptive threshold: 2.0x median but at least 1s (to avoid hyper-aggression on tiny tasks)
        int64_t adaptive = it->second.median_ms * 2;
        return std::max(adaptive, static_cast<int64_t>(1000));
    }

private:
    std::map<std::string, FunctionStats> stats_;
    std::mutex mu_;
};

} // namespace orion::distributed
