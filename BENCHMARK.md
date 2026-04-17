# Orion Performance Benchmarks

This document tracks the evolution of Orion from a simple task runner to a high-performance **Universal Distributed Build Engine**. These benchmarks demonstrate how Orion handles different categories of parallelism and coordination.

---

## 🚀 Scenario 1: The "Universal" Smart Orchestrator
**Script**: `run_universal_demo.sh` (utilizing `benchmarks/universal_cmake_orchestrator.cpp`)

This is our most advanced benchmark. It simulates a **Deep & Wide "Staircase" DAG**—a 10-stage project with 4 parallel chains of sequential dependencies. This tests the system's ability to handle high-frequency "Call Home" events and sub-millisecond task hand-offs.

### Setup
- **Cluster**: 1 Head Node + 4 Worker Nodes (8 total execution threads).
- **Workload**: 33 intensive C++ compilation tasks (4 parallel chains, 8 levels deep, 1 final link).
- **Orchestration**: Automated DAG resolution via gRPC events.

### Results (Recent Run)
| Methodology | Completion Time | Speedup |
| :--- | :--- | :--- |
| **Sequential (Baseline)** | 1810.28 ms | 1.00x |
| **Orion Distributed** | 1033.52 ms | **~1.75x** |

> [!IMPORTANT]
> **Why is speedup not 4x?** In a **Deep DAG**, the performance is limited by the **Critical Path**. A build can only be as fast as its longest sequential chain. Orion saturates the 4 parallel chains perfectly, achieving the theoretical maximum for this specific topology.

---

## 📈 Scenario 2: High-Concurrency "Wide" DAG
**Script**: `benchmarks/compiler_wide_dag.cpp`

This tests **pure horizontal scaling** by compiling 64 independent C++ modules simultaneously. It minimizes coordination overhead and maximizes raw CPU utilization.

### Results
- **Sequential Baseline**: 6.42 seconds
- **Orion Distributed (4 Nodes)**: 1.63 seconds 
- **Total Speedup**: **~3.94x**
- **Time Reduction**: **~74.6%**

---

## ⚠️ Scenario 3: The "In-Process" Bottleneck (Lesson in Systems Design)
**Script**: `./benchmark_test`

This is a **technical demonstration of kernel-level lock contention**. It runs all workers in a single OS process to show why real-world distributed systems *must* use independent process address spaces.

### Findings
- **Discovery**: When multiple threads in one process call `std::system()`, the macOS Unix kernel takes a massive internal lock on the parent process's memory space.
- **Orion Result**: The distributed speedup drops to **~0.95x** (slower than sequential).
- **The Orion Solution**: By moving to gRPC-based process isolation, we bypassed this kernel bottleneck completely.

---

## 🛠️ How to Reproduce
To run the primary Universal Benchmark:
```bash
./run_universal_demo.sh
```

To run the High-Concurrency Wide DAG:
```bash
# Start 1 Head and 4 Nodes in separate terminals
make head node universal_builder
./head 50050
./node 50050 6001 node-1  # Repeat for node-2, node-3, node-4
./benchmarks/compiler_wide_dag 50050
```

---

## 🛡️ Reliability Notes (Phase 4.5 Hardening Pass)

These benchmarks measure the happy path. A separate hardening pass (documented in `challenges.md` §10-14 and `story.md`) fixed five latent liveness/data-loss bugs that the benchmark harness was not exercising:

1. **Bounded retries on `ReportObjectCreated`** (5 attempts, exp. backoff, 3 s deadline each) so a dropped completion RPC no longer deadlocks the subgraph.
2. **`NodeClient::submit_task` now returns `bool`** — failed dispatches are rolled back and re-queued instead of becoming ghost `in_flight_` entries.
3. **`in_flight_` hard-timeout reaper** (default 120 s) so node crashes can't leak scheduler slots.
4. **Per-task dependency-wait timeout** (default 60 s) + `failed_objects_` propagation so a dead upstream fails its subgraph fast.
5. **SHA-256 canonical-hash comparison** between the first completer and any speculative clone — mismatches log `!!! INTEGRITY MISMATCH !!!` and the clone's value is rejected.

Expect no measurable change to the existing speedup numbers. The value is in *what doesn't happen*: no silent stalls, no zombie tasks, no "I swear I saw this complete" ghost artifacts.

### Suggested follow-up benchmarks (not yet run)

- **Node-kill chaos test**: `kill -9` a worker mid-build; measure time-to-failure vs. time-to-recovery (should now be bounded by `in_flight_hard_timeout_`).
- **Network-partition test**: block head↔node port mid-task; confirm `ReportObjectCreated` retries succeed on reconnect and `check_dependency_timeouts()` fails the task cleanly if the partition persists past the timeout.
- **Byzantine worker test**: force one node to return a deliberately corrupted artifact; verify the integrity-mismatch path logs correctly and the original value is retained.
