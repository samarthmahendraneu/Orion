//
// metrics.h — Header-only Prometheus-compatible metrics registry for Orion.
//
// Why not prometheus-cpp?
//   prometheus-cpp is the "right" answer long-term, but it's another
//   external dependency and we don't need its full feature set (histograms,
//   gauges, push gateway, label cardinality control, etc.) yet. The metrics
//   we care about for production readiness are simple integer counters:
//
//     orion_tasks_submitted_total
//     orion_tasks_dispatched_total
//     orion_tasks_completed_total
//     orion_tasks_failed_total
//     orion_dispatch_failures_total
//     orion_retries_total
//     orion_deadletters_total
//     orion_speculative_launched_total
//     orion_speculative_wins_total
//     orion_integrity_mismatches_total
//     orion_heartbeats_received_total
//     orion_nodes_evicted_total
//     orion_dep_timeouts_total
//     orion_in_flight_hard_timeouts_total
//     orion_cancels_sent_total
//
// Singleton design is deliberate. There is exactly one head process per
// cluster, and counters are shared across all services (RPC impls,
// scheduler, HTTP introspection). Threading the registry as a reference
// through every constructor would be ceremony for no win.
//
// Concurrency: std::atomic<uint64_t> under the hood; the registry map is
// guarded by a mutex only on first insertion. Hot-path increments are
// lock-free.
//
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace orion::observability {

class Counter {
public:
    explicit Counter(std::string name, std::string help)
        : name_(std::move(name)), help_(std::move(help)), value_(0) {}

    void inc(uint64_t delta = 1) noexcept { value_.fetch_add(delta, std::memory_order_relaxed); }
    uint64_t value() const noexcept { return value_.load(std::memory_order_relaxed); }

    const std::string& name() const noexcept { return name_; }
    const std::string& help() const noexcept { return help_; }

private:
    std::string name_;
    std::string help_;
    std::atomic<uint64_t> value_;
};

class Metrics {
public:
    static Metrics& instance() {
        static Metrics m;
        return m;
    }

    // Look up (or create) a counter by name. First-call wins on help-text;
    // subsequent calls with the same name return the same counter.
    Counter& counter(const std::string& name, const std::string& help = "") {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = counters_.find(name);
        if (it != counters_.end()) return *it->second;
        auto c = std::make_unique<Counter>(name, help);
        auto* raw = c.get();
        counters_[name] = std::move(c);
        return *raw;
    }

    // Render the registry in Prometheus text-exposition format
    // (https://prometheus.io/docs/instrumenting/exposition_formats/).
    // Safe to call concurrently with increments.
    std::string render_prometheus() const {
        std::lock_guard<std::mutex> lock(mu_);
        std::ostringstream os;
        for (const auto& [name, c] : counters_) {
            if (!c->help().empty()) {
                os << "# HELP " << name << ' ' << c->help() << '\n';
            }
            os << "# TYPE " << name << " counter\n";
            os << name << ' ' << c->value() << '\n';
        }
        return os.str();
    }

    // Cheap JSON for the /cluster endpoint.
    std::string render_json() const {
        std::lock_guard<std::mutex> lock(mu_);
        std::ostringstream os;
        os << '{';
        bool first = true;
        for (const auto& [name, c] : counters_) {
            if (!first) os << ',';
            os << '"' << name << "\":" << c->value();
            first = false;
        }
        os << '}';
        return os.str();
    }

private:
    Metrics() = default;
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::unique_ptr<Counter>> counters_;
};

// ---- Convenience names so callers don't typo metric names ----------------
namespace counters {
    inline Counter& tasks_submitted() {
        return Metrics::instance().counter("orion_tasks_submitted_total",
            "Tasks accepted by the head scheduler");
    }
    inline Counter& tasks_dispatched() {
        return Metrics::instance().counter("orion_tasks_dispatched_total",
            "Tasks successfully handed to a node via NodeClient::submit_task");
    }
    inline Counter& tasks_completed() {
        return Metrics::instance().counter("orion_tasks_completed_total",
            "Tasks whose outputs were recorded in the global object store");
    }
    inline Counter& tasks_failed() {
        return Metrics::instance().counter("orion_tasks_failed_total",
            "Tasks marked permanently failed (any cause)");
    }
    inline Counter& dispatch_failures() {
        return Metrics::instance().counter("orion_dispatch_failures_total",
            "NodeClient::submit_task returned false (RPC or null-stub)");
    }
    inline Counter& retries() {
        return Metrics::instance().counter("orion_retries_total",
            "Tasks requeued after a dispatch failure or node eviction");
    }
    inline Counter& deadletters() {
        return Metrics::instance().counter("orion_deadletters_total",
            "Tasks dropped to the DLQ after exhausting retry budget");
    }
    inline Counter& speculative_launched() {
        return Metrics::instance().counter("orion_speculative_launched_total",
            "Speculative clones dispatched due to straggler detection");
    }
    inline Counter& speculative_wins() {
        return Metrics::instance().counter("orion_speculative_wins_total",
            "Speculative clones that beat the original (by successful completion order)");
    }
    inline Counter& integrity_mismatches() {
        return Metrics::instance().counter("orion_integrity_mismatches_total",
            "SHA-256 hash mismatches between canonical and speculative reports");
    }
    inline Counter& heartbeats_received() {
        return Metrics::instance().counter("orion_heartbeats_received_total",
            "Heartbeat RPCs received by the head");
    }
    inline Counter& nodes_evicted() {
        return Metrics::instance().counter("orion_nodes_evicted_total",
            "Nodes evicted for exceeding heartbeat TTL");
    }
    inline Counter& dep_timeouts() {
        return Metrics::instance().counter("orion_dep_timeouts_total",
            "Tasks failed because a dependency never materialised in time");
    }
    inline Counter& in_flight_hard_timeouts() {
        return Metrics::instance().counter("orion_in_flight_hard_timeouts_total",
            "In-flight tasks reaped after exceeding the hard timeout");
    }
    inline Counter& cancels_sent() {
        return Metrics::instance().counter("orion_cancels_sent_total",
            "CancelTask RPCs sent by the head");
    }
} // namespace counters

} // namespace orion::observability
