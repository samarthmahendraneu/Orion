//
// Created by Samarth Mahendra on 2/27/26.
//
// Reliability hardening (previous revision):
//   * Dispatch failures no longer lie about success
//   * in_flight_ hard-timeout sweep
//   * Dependency-wait timeout with deadletter propagation
//   * SHA-256 "Poisonous Worker" verification (first-writer-wins)
//
// Resilience phase-1 additions (this revision):
//   * Per-task retry budget with jittered exponential backoff
//   * Dead-letter set for tasks that exhaust the budget
//   * NodeRegistry eviction callback -> scheduler requeues in-flight tasks
//     on the evicted node
//   * Speculative cancellation: when a speculative clone wins, the losing
//     node/task-id is queued for the caller to CancelTask on
//   * Structured JSON logs + counters on every interesting transition
//

#include "cluster_scheduler.h"

#include <future>
#include <iostream>
#include <thread>
#include <utility>

#include "../observability/logger.h"
#include "../observability/metrics.h"

namespace orion::distributed {

ClusterScheduler::ClusterScheduler(NodeRegistry& registry, NodeClient& client)
    : registry_(registry),
      client_(client),
      rng_(static_cast<uint64_t>(
          std::chrono::steady_clock::now().time_since_epoch().count()))
{
    // Wire eviction → scheduler. Safe to do in the ctor because the callback
    // only references `this`, and the registry will not fire it until
    // sweep_expired() is called from the monitor thread we haven't started yet.
    registry_.set_eviction_callback(
        [this](const std::string& node_id) { this->on_node_evicted(node_id); });
}

ClusterScheduler::~ClusterScheduler() {
    stop_background_monitoring();
    // Unhook the callback so a late sweep doesn't touch a freed `this`.
    registry_.set_eviction_callback(nullptr);
}

orion::ObjectRef ClusterScheduler::submit(orion::Task task) {
    orion::ObjectRef out{task.id};
    std::vector<std::pair<std::string, orion::Task>> to_dispatch;

    {
        std::lock_guard<std::mutex> lock(mu_);
        submit_time_.try_emplace(task.id, std::chrono::steady_clock::now());
        attempt_.try_emplace(task.id, 1);
        LOG_INFO("ClusterHead", "task_submitted",
                 {"task_id", task.id},
                 {"fn", task.function_name});
        observability::counters::tasks_submitted().inc();
        pending_.push(std::move(task));
        to_dispatch = plan_dispatches_internal_();
    }

    dispatch_and_handle_failures_(std::move(to_dispatch));
    return out;
}

void ClusterScheduler::schedule() {
    std::vector<std::pair<std::string, orion::Task>> to_dispatch;

    {
        std::lock_guard<std::mutex> lock(mu_);
        to_dispatch = plan_dispatches_internal_();
    }

    dispatch_and_handle_failures_(std::move(to_dispatch));
}

std::vector<std::pair<std::string, orion::Task>>
ClusterScheduler::plan_dispatches_internal_() {
    std::vector<std::pair<std::string, orion::Task>> to_dispatch;
    const auto now = std::chrono::steady_clock::now();

    int q_size = static_cast<int>(pending_.size());
    for (int i = 0; i < q_size; i++) {
        orion::Task task = std::move(pending_.front());
        pending_.pop();

        // Deadletter propagation.
        if (deps_failed_internal_(task)) {
            LOG_ERROR("ClusterHead", "task_failed_upstream",
                      {"task_id", task.id});
            failed_objects_.insert(task.id);
            submit_time_.erase(task.id);
            attempt_.erase(task.id);
            not_before_.erase(task.id);
            observability::counters::tasks_failed().inc();
            continue;
        }

        if (!deps_ready_internal_(task)) {
            pending_.push(std::move(task));
            continue;
        }

        // Retry backoff: don't dispatch before not_before_[task.id].
        auto nb_it = not_before_.find(task.id);
        if (nb_it != not_before_.end() && now < nb_it->second) {
            pending_.push(std::move(task));
            continue;
        }

        auto node_opt = registry_.pick_node();
        if (!node_opt) {
            pending_.push(std::move(task));
            continue;
        }

        InFlightTask ift;
        ift.task = task;
        ift.node_id = node_opt->node_id;
        ift.start_time = now;
        ift.is_speculative = false;
        ift.attempt = attempt_.count(task.id) ? attempt_[task.id] : 1;
        in_flight_[task.id] = ift;

        // Clear the backoff entry now that we're actually dispatching.
        not_before_.erase(task.id);

        to_dispatch.emplace_back(node_opt->node_id, std::move(task));
    }
    return to_dispatch;
}

void ClusterScheduler::dispatch_and_handle_failures_(
    std::vector<std::pair<std::string, orion::Task>> to_dispatch)
{
    if (to_dispatch.empty()) return;

    // Fire all ExecuteTask RPCs in parallel so a batch of N ready tasks
    // (e.g. 12 independent compile steps) takes one RPC round-trip instead
    // of N sequential round-trips.  Each future captures a copy of its task-id
    // and node-id; the task object itself is moved into the lambda.
    struct DispatchResult { std::string task_id; std::string node_id; bool ok; };
    std::vector<std::future<DispatchResult>> futures;
    futures.reserve(to_dispatch.size());

    for (auto& [node_id, task] : to_dispatch) {
        // V2: Resolve dependency hashes for the input_map
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& dep : task.deps) {
            auto it = canonical_hashes_.find(dep.id);
            if (it != canonical_hashes_.end()) {
                task.input_map[dep.id] = it->second;
            }
        }

        std::string tid = task.id;
        std::string nid = node_id;
        futures.emplace_back(
            std::async(std::launch::async,
                [this, tid, nid, t = std::move(task)]() mutable -> DispatchResult {
                    bool ok = client_.submit_task(nid, std::move(t));
                    return {tid, nid, ok};
                })
        );
    }

    for (auto& fut : futures) {
        auto [task_id_copy, node_id_copy, ok] = fut.get();

        if (ok) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                dispatch_time_[task_id_copy] = std::chrono::steady_clock::now();
            }
            observability::counters::tasks_dispatched().inc();
            LOG_INFO("ClusterHead", "task_dispatched",
                     {"task_id", task_id_copy},
                     {"node_id", node_id_copy});
            continue;
        }

        observability::counters::dispatch_failures().inc();
        LOG_ERROR("ClusterHead", "dispatch_failed",
                  {"task_id", task_id_copy},
                  {"node_id", node_id_copy});

        registry_.remove_node(node_id_copy); // Ensure we don't try this node again until it re-registers

        std::lock_guard<std::mutex> lock(mu_);
        auto it = in_flight_.find(task_id_copy);
        if (it == in_flight_.end()) {
            continue;
        }
        orion::Task kicked_back = std::move(it->second.task);
        const int attempt_done = it->second.attempt;
        in_flight_.erase(it);
        requeue_or_deadletter_locked_(std::move(kicked_back), attempt_done,
                                      "dispatch-failed");
    }
}

void ClusterScheduler::requeue_or_deadletter_locked_(orion::Task task,
                                                     int attempt,
                                                     const std::string& reason) {
    const std::string id = task.id;
    if (attempt >= max_attempts_) {
        LOG_CRITICAL("ClusterHead", "task_dead_lettered",
                     {"task_id", id},
                     {"attempts", std::to_string(attempt)},
                     {"reason", reason});
        dead_letter_.insert(id);
        failed_objects_.insert(id);
        attempt_.erase(id);
        submit_time_.erase(id);
        not_before_.erase(id);
        observability::counters::deadletters().inc();
        observability::counters::tasks_failed().inc();
        return;
    }

    const int next_attempt = attempt + 1;
    attempt_[id] = next_attempt;

    // Jittered exponential backoff: base * 2^(attempt-1) * uniform(0.5, 1.5).
    // Capped at 30s so a string of failures doesn't push retry into next week.
    const int exp = std::max(0, attempt - 1);
    const auto raw = retry_base_backoff_ * (1LL << std::min(exp, 20));
    std::uniform_real_distribution<double> jitter(0.5, 1.5);
    const auto dur = std::chrono::milliseconds(
        static_cast<long long>(raw.count() * jitter(rng_)));
    const auto capped = std::min(dur, std::chrono::milliseconds(30'000));
    not_before_[id] = std::chrono::steady_clock::now() + capped;

    LOG_WARN("ClusterHead", "task_retry_scheduled",
             {"task_id", id},
             {"attempt", std::to_string(next_attempt)},
             {"backoff_ms", std::to_string(capped.count())},
             {"reason", reason});
    observability::counters::retries().inc();
    pending_.push(std::move(task));
}

void ClusterScheduler::put_object(const std::string& object_id, std::any value) {
    put_object_with_hash(object_id, std::move(value), "");
}

void ClusterScheduler::put_object_with_hash(const std::string& object_id,
                                            std::any value,
                                            const std::string& hash) {
    std::vector<std::pair<std::string, orion::Task>> to_dispatch;
    {
        std::lock_guard<std::mutex> lock(mu_);

        const bool already_finished =
            global_objects_.find(object_id) != global_objects_.end();

        if (!already_finished) {
            if (!hash.empty()) {
                canonical_hashes_[object_id] = hash;
                LOG_INFO("ClusterHead", "canonical_hash_recorded",
                         {"object_id", object_id},
                         {"hash_prefix", hash.substr(0, 16)});
            }
            global_objects_[object_id] = std::move(value);
            attempt_.erase(object_id);
            observability::counters::tasks_completed().inc();
        } else {
            auto can_it = canonical_hashes_.find(object_id);
            if (can_it != canonical_hashes_.end() && !hash.empty()) {
                if (hash != can_it->second) {
                    LOG_CRITICAL("ClusterHead", "integrity_mismatch",
                                 {"object_id", object_id},
                                 {"canonical", can_it->second.substr(0, 16)},
                                 {"reported",  hash.substr(0, 16)});
                    observability::counters::integrity_mismatches().inc();
                } else {
                    LOG_INFO("ClusterHead", "speculative_clone_verified",
                             {"object_id", object_id});
                }
            } else if (!hash.empty() && can_it == canonical_hashes_.end()) {
                canonical_hashes_[object_id] = hash;
            }
        }

        // Bookkeeping: if an in_flight entry still exists, this is the LATER
        // reporter (speculative loser). Queue a cancel for them.
        auto it = in_flight_.find(object_id);
        if (it != in_flight_.end()) {
            if (already_finished) {
                // We're the loser or a late duplicate — tell the node to stop.
                pending_cancels_.push_back(PendingCancel{
                    it->second.node_id, object_id, "speculative-loser"});
                observability::counters::cancels_sent().inc();
                // A speculative clone beat the original → count the win.
                if (it->second.is_speculative) {
                    observability::counters::speculative_wins().inc();
                }
            } else if (hash.empty()
                       && it->second.task.function_name == "shell_execute") {
                LOG_WARN("ClusterHead", "shell_execute_no_hash",
                         {"object_id", object_id});
            }
            in_flight_.erase(it);
        }
        submit_time_.erase(object_id);

        if (already_finished) {
            return;
        }

        to_dispatch = plan_dispatches_internal_();
    }

    dispatch_and_handle_failures_(std::move(to_dispatch));
}

void ClusterScheduler::check_speculative_execution() {
    std::vector<std::pair<std::string, orion::Task>> clones_to_dispatch;

    {
        std::lock_guard<std::mutex> lock(mu_);
        auto now = std::chrono::steady_clock::now();

        for (auto& [id, ift] : in_flight_) {
            if (ift.is_speculative) continue;

            auto duration_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - ift.start_time).count();
            
            int64_t threshold_ms = latency_tracker_.get_straggler_threshold_ms(
                ift.task.function_name, 
                std::chrono::duration_cast<std::chrono::milliseconds>(straggler_threshold_).count());

            if (duration_ms > threshold_ms) {
                auto node_opt = registry_.pick_node();
                if (node_opt && node_opt->node_id != ift.node_id) {
                    LOG_WARN("ClusterHead", "speculative_launched",
                             {"task_id", id},
                             {"fn", ift.task.function_name},
                             {"original_node", ift.node_id},
                             {"clone_node", node_opt->node_id},
                             {"age_ms", std::to_string(duration_ms)},
                             {"threshold_ms", std::to_string(threshold_ms)});
                    observability::counters::speculative_launched().inc();
                    ift.is_speculative = true;
                    clones_to_dispatch.emplace_back(node_opt->node_id, ift.task);
                }
            }
        }
    }

    dispatch_and_handle_failures_(std::move(clones_to_dispatch));
}

void ClusterScheduler::check_dependency_timeouts() {
    bool anything_failed = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto now = std::chrono::steady_clock::now();

        // 1) Pending tasks waiting on missing deps.
        std::queue<orion::Task> new_pending;
        while (!pending_.empty()) {
            orion::Task t = std::move(pending_.front());
            pending_.pop();

            const bool dep_failed = deps_failed_internal_(t);

            auto st_it = submit_time_.find(t.id);
            const bool expired =
                (st_it != submit_time_.end())
                && !deps_ready_internal_(t)
                && (now - st_it->second > dep_timeout_);

            if (dep_failed || expired) {
                LOG_ERROR("ClusterHead", "task_failed_timeout",
                          {"task_id", t.id},
                          {"cause", dep_failed ? "upstream-failed" : "dep-wait-timeout"});
                failed_objects_.insert(t.id);
                submit_time_.erase(t.id);
                attempt_.erase(t.id);
                not_before_.erase(t.id);
                if (!dep_failed) observability::counters::dep_timeouts().inc();
                observability::counters::tasks_failed().inc();
                anything_failed = true;
            } else {
                new_pending.push(std::move(t));
            }
        }
        pending_ = std::move(new_pending);

        // 2) In-flight tasks past the hard timeout — assume node crashed.
        std::vector<std::string> reap;
        for (const auto& [id, ift] : in_flight_) {
            if (now - ift.start_time > in_flight_hard_timeout_) {
                reap.push_back(id);
            }
        }
        for (const auto& id : reap) {
            LOG_ERROR("ClusterHead", "in_flight_hard_timeout",
                      {"task_id", id},
                      {"node_id", in_flight_[id].node_id});
            observability::counters::in_flight_hard_timeouts().inc();
            // Unlike dep-timeout, an in-flight task that crashed can still be
            // retried on another node. Respect the retry budget.
            orion::Task kicked = std::move(in_flight_[id].task);
            const int attempt_done = in_flight_[id].attempt;
            in_flight_.erase(id);
            requeue_or_deadletter_locked_(std::move(kicked), attempt_done,
                                          "in-flight-timeout");
            anything_failed = true;
        }
    }

    if (anything_failed) {
        schedule();
    }
}

void ClusterScheduler::on_task_failed_report(const std::string& task_id) {
    // Called when a node explicitly signals that it could not complete a task
    // (e.g. shell_execute exhausted its retry budget).  We move the task from
    // in_flight_ → pending_ immediately (subject to the retry budget) so the
    // head dispatches it to another node without waiting for the
    // in_flight_hard_timeout_ sweep.
    std::vector<std::pair<std::string, orion::Task>> to_dispatch;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = in_flight_.find(task_id);
        if (it == in_flight_.end()) {
            // Already handled (e.g., hard-timeout fired first). Nothing to do.
            LOG_WARN("ClusterHead", "task_failed_report_stale",
                     {"task_id", task_id});
            return;
        }
        orion::Task kicked_back = std::move(it->second.task);
        const int attempt_done  = it->second.attempt;
        in_flight_.erase(it);
        submit_time_.erase(task_id);
        observability::counters::tasks_failed().inc();
        requeue_or_deadletter_locked_(std::move(kicked_back), attempt_done,
                                      "node-reported-failure");
        to_dispatch = plan_dispatches_internal_();
    }
    dispatch_and_handle_failures_(std::move(to_dispatch));
}

void ClusterScheduler::on_node_evicted(const std::string& node_id) {
    // This runs outside NodeRegistry's mutex (by design), so we're free to
    // take our own.
    std::vector<std::string> affected;
    {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<std::string> to_requeue_ids;
        for (const auto& [id, ift] : in_flight_) {
            if (ift.node_id == node_id) to_requeue_ids.push_back(id);
        }
        for (const auto& id : to_requeue_ids) {
            orion::Task t = std::move(in_flight_[id].task);
            const int attempt_done = in_flight_[id].attempt;
            in_flight_.erase(id);
            affected.push_back(id);
            requeue_or_deadletter_locked_(std::move(t), attempt_done,
                                          "node-evicted");
        }
    }

    LOG_WARN("ClusterHead", "node_eviction_reclaim",
             {"node_id", node_id},
             {"requeued_count", std::to_string(affected.size())});

    schedule();
}

bool ClusterScheduler::is_failed(const std::string& object_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    return failed_objects_.count(object_id) > 0;
}

std::vector<PendingCancel> ClusterScheduler::take_pending_cancels() {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<PendingCancel> out;
    out.swap(pending_cancels_);
    return out;
}

ClusterScheduler::Snapshot ClusterScheduler::snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    Snapshot s;
    s.pending = pending_.size();
    s.in_flight = in_flight_.size();
    s.failed = failed_objects_.size();
    s.dead_lettered = dead_letter_.size();
    s.canonical_hashes = canonical_hashes_.size();
    return s;
}

std::optional<std::any> ClusterScheduler::get_object(const std::string& object_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = global_objects_.find(object_id);
    if (it != global_objects_.end()) {
        return it->second;
    }
    return std::nullopt;
}

bool ClusterScheduler::deps_ready_internal_(const orion::Task& task) const {
    for (const auto& dep : task.deps) {
        if (global_objects_.find(dep.id) == global_objects_.end()) {
            return false;
        }
    }
    return true;
}

bool ClusterScheduler::deps_failed_internal_(const orion::Task& task) const {
    for (const auto& dep : task.deps) {
        if (failed_objects_.count(dep.id)) return true;
    }
    return false;
}

void ClusterScheduler::start_background_monitoring() {
    monitor_thread_ = std::make_unique<std::jthread>([this](std::stop_token st) {
        while (!st.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (st.stop_requested()) break;
            // Eviction sweep first so downstream paths see the fallout in
            // the same tick.
            registry_.sweep_expired(registry_.node_ttl());
            check_speculative_execution();
            check_dependency_timeouts();
            schedule();
        }
    });
}

void ClusterScheduler::stop_background_monitoring() {
    if (monitor_thread_) {
        monitor_thread_->request_stop();
        monitor_thread_.reset();
    }
}

void ClusterScheduler::apply_log_entry(const orion::OrionLogEntry& entry) {
    std::lock_guard<std::mutex> lock(mu_);

    switch (entry.op_case()) {
        case orion::OrionLogEntry::kSubmitted: {
            const auto& sub = entry.submitted().task_req();

            // V2: Harvest source artifact hashes
            for (const auto& [filename, hash] : sub.input_map()) {
                if (!hash.empty() && canonical_hashes_.find(filename) == canonical_hashes_.end()) {
                    canonical_hashes_[filename] = hash;
                }
            }

            orion::Task task;
            task.id = sub.task_id();
            task.function_name = sub.function_name();
            for (const auto& dep_id : sub.dep_ids()) {
                task.deps.push_back(orion::ObjectRef{dep_id});
            }
            for (const auto& bytes : sub.args()) {
                task.args.push_back(bytes);
            }
            // Populate task's input_map so dispatch can use it
            for (const auto& [name, hash] : sub.input_map()) {
                task.input_map[name] = hash;
            }

            submit_time_.try_emplace(task.id, std::chrono::steady_clock::now());
            attempt_.try_emplace(task.id, 1);
            if (!entry.submitted().action_hash().empty()) {
                task_to_action_[task.id] = entry.submitted().action_hash();
            }
            pending_.push(std::move(task));
            
            LOG_INFO("ClusterHead", "state_machine_apply_submitted", {{"task_id", sub.task_id()}, {"action_hash", entry.submitted().action_hash()}});
            break;
        }
        case orion::OrionLogEntry::kDispatched: {
            const auto& disp = entry.dispatched();
            // In a real Raft system, we'd need to find the task in pending_ and move to in_flight_
            // For a POC, we can assume the task is at the front or search for it.
            // Simplified: we've already done the planning on the leader.
            break;
        }
        case orion::OrionLogEntry::kCompleted: {
            const auto& comp = entry.completed();
            const std::string& object_id = comp.object_id();
            const std::string& hash = comp.hash();

            if (global_objects_.find(object_id) == global_objects_.end()) {
                if (!hash.empty()) {
                    canonical_hashes_[object_id] = hash;
                }
                attempt_.erase(object_id);
                submit_time_.erase(object_id);
                in_flight_.erase(object_id);
                task_to_action_.erase(object_id);
            }
            LOG_INFO("ClusterHead", "state_machine_apply_completed", {"object_id", object_id});
            break;
        }
        case orion::OrionLogEntry::kFailed: {
            const auto& fail = entry.failed();
            failed_objects_.insert(fail.task_id());
            in_flight_.erase(fail.task_id());
            attempt_.erase(fail.task_id());
            submit_time_.erase(fail.task_id());
            task_to_action_.erase(fail.task_id());
            LOG_INFO("ClusterHead", "state_machine_apply_failed", {"task_id", fail.task_id()});
            break;
        }
        case orion::OrionLogEntry::kCancelled: {
            const auto& canc = entry.cancelled();
            in_flight_.erase(canc.task_id());
            LOG_INFO("ClusterHead", "state_machine_apply_cancelled", {"task_id", canc.task_id()});
            break;
        }
        case orion::OrionLogEntry::kNodeRegistered: {
             const auto& reg = entry.node_registered();
             // Metadata replication for nodes if we want to share the registry.
             break;
        }
        case orion::OrionLogEntry::kNodeEvicted: {
             const auto& evict = entry.node_evicted();
             // Logic to handle eviction in state machine
             break;
        }
        default:
            break;
    }
}

    std::string ClusterScheduler::get_canonical_hash(const std::string& object_id) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = canonical_hashes_.find(object_id);
        if (it != canonical_hashes_.end()) {
            return it->second;
        }
        return "";
    }

    std::string ClusterScheduler::pop_action_for_task(const std::string& task_id) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = task_to_action_.find(task_id);
        if (it != task_to_action_.end()) {
            std::string action_hash = it->second;
            // We usually link it and then we don't need it. 
            // Better to keep it until completed just in case of multiple reports?
            // Actually, once linked, we can erase.
            return action_hash;
        }
        return "";
    }

    void ClusterScheduler::log_performance_metrics(const std::string& task_id) {
        std::lock_guard<std::mutex> lock(mu_);
        auto now = std::chrono::steady_clock::now();
        completion_time_[task_id] = now;
        
        auto it_sub = submit_time_.find(task_id);
        auto it_dis = dispatch_time_.find(task_id);
        
        if (it_sub != submit_time_.end() && it_dis != dispatch_time_.end()) {
            auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - it_sub->second).count();
            auto queue_ms = std::chrono::duration_cast<std::chrono::milliseconds>(it_dis->second - it_sub->second).count();
            auto exec_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - it_dis->second).count();
            
            LOG_INFO("ClusterHead", "performance_metric",
                     {{"task_id", task_id},
                      {"total_ms", std::to_string(total_ms)},
                      {"queue_ms", std::to_string(queue_ms)},
                      {"exec_ms", std::to_string(exec_ms)}});

            // Feed the latency tracker for dynamic speculation
            auto it_if = in_flight_.find(task_id);
            if (it_if != in_flight_.end()) {
                latency_tracker_.record_completion(it_if->second.task.function_name, exec_ms);
            }
        }
    }

} // namespace orion::distributed
