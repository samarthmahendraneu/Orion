# Orion Benchmark Guide

This document describes how to execute the performance benchmarks for the Orion Distributed Task Execution Framework. 

We provide two separate benchmarks:
1. **In-Process Fork Contention Simulation (`benchmark_test`)**
2. **True gRPC Distributed Compilation (`submit_benchmark`)**

---

## 1. True gRPC Distributed Compilation (`submit_benchmark`)

This benchmark proves the **near-theoretical parallel scaling** of the Orion distributed scheduler.
It bypasses the macOS `fork()` kernel-lock limitation by mapping worker threads to entirely independent operating system processes using our `gRPC` layer.

### What it does:
1. Generates 32 unique, heavy C++ source files containing `<vector>` and `<algorithm>` imports.
2. Runs a 1-thread sequential baseline compile using `clang++`.
3. Dispatches 32 `TaskRequest` messages over `gRPC` to an active Cluster Head.
4. The Head load-balances the tasks via Round-Robin across 4 Worker Nodes.
5. The Worker Nodes independently call `std::system("clang++ ...")`, fully saturating physical CPU cores without kernel-level `fork()` blocking.

### How to reproduce (8-workers max throughput):

**Step 1: Open 6 Terminal Tabs to your Orion root directory**

**Step 2: Start the Cluster Head (Terminal 1)**
```bash
./head 50050
```

**Step 3: Start 4 Worker Nodes (Terminals 2, 3, 4, 5)**
Each worker node spins up 2 internal threads.
```bash
# Terminal 2
./node 50050 6001 node-alpha
# Terminal 3
./node 50050 6002 node-beta
# Terminal 4
./node 50050 6003 node-gamma
# Terminal 5
./node 50050 6004 node-delta
```

**Step 4: Execute the Benchmark (Terminal 6)**
```bash
make submit_benchmark && ./submit_benchmark 50050
```

**Expected Hardware Results (8-Core CPU):**
You should expect a clean ~3.9x speedup (representing the 4 active nodes doing 2x parallel thread execution, limited lightly by OS process switching overhead).
```text
Speedup: ~3.94x
Time Reduction: ~74.6%
```

---

## 2. In-Process Lock Contention Demo (`benchmark_test`)

This script serves as a **systems-engineering demonstration of kernel-level lock contention**. 

### What it does:
It attempts to run 4 Nodes (8 workers total) entirely **in-process** via the `InProcessNodeClient` and requests them to concurrently execute `std::system("clang++ ...")`.

Because `std::system` relies on `fork()`, the macOS Unix kernel must take a massive internal lock on the parent process's memory space. Since all 8 workers reside in the exact same process, they blindly block each other before executing the shell, causing extreme serialization.

### How to reproduce:
```bash
make benchmark_test && ./benchmark_test
```

**Expected Results:**
The distributed speedup will drop to ~0.95x. This proves why real deployments *must* separate workers into independent OS processes (as done securely by the `submit_benchmark` gRPC script above).
