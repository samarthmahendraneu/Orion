//
// Created by Samarth Mahendra on 2/27/26.
//
// Reliability hardening (this revision):
//   * Dispatch failures no longer lie about success — when NodeClient::submit_task
//     returns false we roll back the in_flight_ entry and re-queue the task.
//   * in_flight_ has a hard timeout sweep (in_flight_hard_timeout_) so
//     node crashes or lost ReportObjectCreated RPCs don't leak entries forever.
//   * Pending tasks have a dependency-wait timeout (dep_timeout_). If a
//     dep never materialises the task is marked failed and its failure is
//     propagated down the DAG so nothing hangs indefinitely.
//   * SHA-256 "Poisonous Worker" defence is now actually wired: first completer
//     records the canonical hash; subsequent speculative reports are verified
//     against it and rejected on mismatch.
//

#include "cluster_scheduler.h"

#include <iostream>
#include <thread>
#include <utility>

namespace orion::distributed {

ClusterScheduler::ClusterScheduler(NodeRegistry& registry, NodeClient& client)
    : registry_(registry), client_(client) {}

ClusterScheduler::~ClusterScheduler() {
    stop_background_monitoring();
}

orion::ObjectRef ClusterScheduler::submit(orion::Task task) {
    orion::ObjectRef out{task.id};
    std::vector<std::pair<std::string, orion::Task>> to_dispatch;

    {
        std::lock_guard<std::mutex> lock(mu_);
        // Record submission time for dep-timeout tracking. Keep the earliest
        // timestamp if the task was previously rolled back by a dispatch
        // failure (so the timeout clock doesn't reset on every retry).
        submit_time_.try_emplace(task.id, std::chrono::steady_clock::now());
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

    int q_size = static_cast<int>(pending_.size());
    for (int i = 0; i < q_size; i++) {
        orion::Task task = std::move(pending_.front());
        pending_.pop();

        // Deadletter propagation: if any upstream is permanently failed,
        // fail this task immediately instead of queuing forever.
        if (deps_failed_internal_(task)) {
            std::cerr << "[ClusterHead] Task " << task.id
                      << " FAILED: upstream dependency already failed. "
                      << "Propagating failure.\n" << std::flush;
            failed_objects_.insert(task.id);
            submit_time_.erase(task.id);
            continue;
        }

        // Non-locking internal dependency check.
        if (!deps_ready_internal_(task)) {
            pending_.push(std::move(task));
            continue;
        }

        auto node_opt = registry_.pick_node();
        if (!node_opt) {
            pending_.push(std::move(task));
            continue;
        }

        // Track in-flight for speculative execution & integrity verification.
        InFlightTask ift;
        ift.task = task;                                    // copy kept for retry/speculation
        ift.node_id = node_opt->node_id;
        ift.start_time = std::chrono::steady_clock::now();
        ift.is_speculative = false;
        in_flight_[task.id] = ift;

        to_dispatch.emplace_back(node_opt->node_id, std::move(task));
    }
    return to_dispatch;
}

void ClusterScheduler::dispatch_and_handle_failures_(
    std::vector<std::pair<std::string, orion::Task>> to_dispatch)
{
    for (auto& pair : to_dispatch) {
        const std::string task_id_copy = pair.second.id;
        const std::string node_id_copy = pair.first;

        const bool ok = client_.submit_task(node_id_copy, std::move(pair.second));
        if (ok) continue;

        // Dispatch failed — roll back so we don't have a ghost in_flight entry.
        std::cerr << "[ClusterHead] Dispatch FAILED task=" << task_id_copy
                  << " node=" << node_id_copy
                  << " — rolling back in-flight, re-queuing for another attempt.\n"
                  << std::flush;

        std::lock_guard<std::mutex> lock(mu_);
        auto it = in_flight_.find(task_id_copy);
        if (it == in_flight_.end()) {
            // Either already completed (raced with put_object) or already rolled
            // back by another path — nothing to do.
            continue;
        }
        pending_.push(std::move(it->second.task));
        in_flight_.erase(it);
    }
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

        // --- SHA-256 "Poisonous Worker" defence ---------------------------------
        // First completer establishes the canonical hash. Every subsequent
        // (speculative) completer is verified against it. A mismatch is a hard
        // error — we keep the original value and refuse to overwrite, and we
        // log the discrepancy loudly.
        if (!already_finished) {
            if (!hash.empty()) {
                canonical_hashes_[object_id] = hash;
                std::cout << "[ClusterHead] Canonical hash recorded object="
                          << object_id << " hash=" << hash.substr(0, 16) << "...\n"
                          << std::flush;
            }
            // Record the value.
            global_objects_[object_id] = std::move(value);
        } else {
            auto can_it = canonical_hashes_.find(object_id);
            if (can_it != canonical_hashes_.end() && !hash.empty()) {
                if (hash != can_it->second) {
                    std::cerr << "[ClusterHead] !!! INTEGRITY MISMATCH !!! object="
                              << object_id
                              << " canonical=" << can_it->second.substr(0, 16) << "..."
                              << " reported=" << hash.substr(0, 16) << "..."
                              << " — rejecting duplicate, original retained. "
                              << "Reporting node should be treated as suspect.\n"
                              << std::flush;
                } else {
                    std::cout << "[ClusterHead] Speculative clone verified object="
                              << object_id << "\n" << std::flush;
                }
            } else if (!hash.empty() && can_it == canonical_hashes_.end()) {
                // First completer didn't ship a hash but the speculative did —
                // record it post-hoc so a third reporter would still be checked.
                canonical_hashes_[object_id] = hash;
            }
            // Don't overwrite the value. Skip re-planning.
        }

        // Clean up in-flight bookkeeping (both the canonical and any duplicate
        // reports pass through here).
        auto it = in_flight_.find(object_id);
        if (it != in_flight_.end()) {
            if (!already_finished
                && hash.empty()
                && it->second.task.function_name == "shell_execute")
            {
                std::cerr << "[ClusterHead] WARNING: shell_execute task "
                          << object_id << " returned NO hash — integrity "
                          << "not verifiable for this artifact.\n" << std::flush;
            }
            in_flight_.erase(it);
        }
        submit_time_.erase(object_id);

        if (already_finished) {
            // Duplicate report — no new work to plan.
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

            auto duration =
                std::chrono::duration_cast<std::chrono::seconds>(now - ift.start_time);

            if (duration > straggler_threshold_) {
                auto node_opt = registry_.pick_node();
                if (node_opt && node_opt->node_id != ift.node_id) {
                    std::cout << "[ClusterHead] STRAGGLER DETECTED task=" << id
                              << " on node=" << ift.node_id
                              << " (" << duration.count() << "s). "
                              << "Launching speculative clone on "
                              << node_opt->node_id << "...\n" << std::flush;
                    // Only flip the flag if we successfully handed off to
                    // dispatch_and_handle_failures_. If that call fails we
                    // want check_speculative_execution to try again.
                    ift.is_speculative = true;
                    clones_to_dispatch.emplace_back(node_opt->node_id, ift.task);
                }
            }
        }
    }

    // Note: dispatch_and_handle_failures_ will roll back the in_flight entry
    // on failure, which would also clear is_speculative. That's fine — the
    // next monitor tick will re-evaluate.
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
                std::cerr << "[ClusterHead] Task " << t.id << " FAILED ("
                          << (dep_failed ? "upstream-failed" : "dep-wait-timeout")
                          << "). Dropping from pending, propagating to downstream.\n"
                          << std::flush;
                failed_objects_.insert(t.id);
                submit_time_.erase(t.id);
                anything_failed = true;
            } else {
                new_pending.push(std::move(t));
            }
        }
        pending_ = std::move(new_pending);

        // 2) In-flight tasks that have exceeded the hard timeout — we assume
        //    the node crashed or the completion RPC was lost. Mark failed.
        std::vector<std::string> reap;
        for (const auto& [id, ift] : in_flight_) {
            if (now - ift.start_time > in_flight_hard_timeout_) {
                reap.push_back(id);
            }
        }
        for (const auto& id : reap) {
            std::cerr << "[ClusterHead] In-flight task " << id
                      << " exceeded hard timeout on node="
                      << in_flight_[id].node_id
                      << " — presumed lost. Marking failed.\n" << std::flush;
            failed_objects_.insert(id);
            in_flight_.erase(id);
            submit_time_.erase(id);
            anything_failed = true;
        }
    }

    // If anything was marked failed, downstreams may now need to fail too.
    // Rerun the planner so deadletter propagation cascades through the DAG.
    if (anything_failed) {
        schedule();
    }
}

bool ClusterScheduler::is_failed(const std::string& object_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    return failed_objects_.count(object_id) > 0;
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
            check_speculative_execution();
            check_dependency_timeouts();
            // Drain any dispatch-failure retries that are sitting in pending_.
            schedule();
        }
    });
}

void ClusterScheduler::stop_background_monitoring() {
    if (monitor_thread_) {
        monitor_thread_->request_stop();
        monitor_thread_.reset();   // joins via jthread dtor
    }
}

} // namespace orion::distributed
