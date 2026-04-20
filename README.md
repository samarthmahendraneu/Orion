# Orion

A multi-threaded, dependency-aware task execution framework in C++23 — designed to scale from a single-machine runtime to a distributed cluster of worker nodes.

## Overview

Orion models computation as a **dataflow graph**: tasks declare their inputs as `ObjectRef` dependencies, and the scheduler dispatches them to workers only when all dependencies are satisfied. The framework is layered into three tiers:

| Tier | What it does |
|---|---|
| **Core** | Tasks, workers, scheduler, object store — the high-performance engine |
| **Local** | `Runtime` — a clean façade over the core for single-process use |
| **Distributed** | `NodeRuntime`, `ClusterScheduler`, `NodeRegistry`, `NodeClient` — gRPC-based cluster orchestration |
| **V2 Engine** | **Raft Replication**, **Merkle Action Cache**, **Distributed CAS** |
| **Hardened** | **Speculative Execution** (Straggler Mitigation) & **SHA-256 Integrity Verification** |
| **Real-World** | **Redis Compilation Scaling** (5.6x speedup on 6 nodes) & **Global Context Uplift** |

---

## Architecture

### Core Layer (`src/core/`)

<img width="1512" height="1314" alt="image" src="https://github.com/user-attachments/assets/cbbf2a5f-81b9-4fb3-bd69-4b6181a546b5" />

https://excalidraw.com/#json=vnPHTMiI-ZkdczEkcpeKh,bthOxQcgldqqnRsoox0SSg


## Project Structure

```
Orion/
├── src/
│   ├── main.cpp                          # Entry point / integration demo
│   ├── core/
│   │   ├── task.h                        # Task struct
│   │   ├── object_ref.h                  # ObjectRef / ObjectId
│   │   ├── object_store.{h,cpp}          # Thread-safe result store
│   │   ├── worker.{h,cpp}                # Background-thread executor
│   │   └── scheduler.{h,cpp}             # Local dataflow scheduler
│   ├── local/
│   │   └── runtime.{h,cpp}               # Single-process Runtime façade
│   └── distributed/
│       ├── node_runtime.{h,cpp}          # Per-node runtime wrapper
│       ├── cluster/
│       │   ├── node_registry.{h,cpp}     # Cluster membership + node selection
│       │   └── cluster_scheduler.{h,cpp} # Cross-node dataflow scheduler
│       ├── rpc/
│       │   ├── node_client.h             # Abstract RPC interface
│       │   ├── inprocess_node_client.h   # In-process stub (testing)
│       │   └── grpc_node_client.h        # Real gRPC transport implementation
│       └── proto/
│           └── orion.proto               # cluster communication definitions
├── head_main.cpp                         # Cluster Head server entry point
├── node_main.cpp                         # Worker Node entry point
├── submit_test.cpp                       # gRPC task submission test
├── Makefile
└── LICENSE
```

---

#### Task (`task.h`)

The fundamental unit of work.

```cpp
struct Task {
    std::string id;                           // unique identifier / output key
    std::vector<ObjectRef> deps;              // IDs of required input objects
    std::function<std::any(const std::vector<std::any>&)> work;
};
```

#### ObjectRef / ObjectId (`object_ref.h`)

A lightweight handle to a future or present result stored in the `ObjectStore`.

```cpp
struct ObjectRef { ObjectId id; };
```

#### ObjectStore (`object_store.h/cpp`)

Thread-safe, in-memory key-value store for task results.

| Method | Behaviour |
|---|---|
| `put(id, value)` | Store a result; triggers the registered callback |
| `get(id)` | Non-blocking; returns `std::nullopt` if absent |
| `get_blocking(id)` | Blocks until the value is available |
| `set_on_put_callback(fn)` | Notify scheduler when a new object lands |

#### Worker (`worker.h/cpp`)

Owns a single background thread. Dequeues tasks, resolves dependency values from the object store, and invokes `task.work`. Supports **work-stealing friendly** queueing via mutex + condition variable.

| Method | Behaviour |
|---|---|
| `submit(task)` → `ObjectRef` | Enqueue a task; returns its output ref |
| `start()` / `stop()` | Lifecycle control |

#### Scheduler (`scheduler.h/cpp`)

Dataflow scheduler that sits between callers and workers.

- Tracks all submitted tasks in a `pending` map
- When `on_object_created` fires, re-evaluates readiness of waiting tasks
- Dispatches ready tasks to workers via **round-robin**

---

### Local Layer (`src/local/`)

#### Runtime (`runtime.h/cpp`)

A single-process, batteries-included entry point. Owns the object store, N workers, and the scheduler—hiding all wiring from the caller.

```cpp
orion::Runtime rt(4);           // 4 worker threads

orion::Task t{"square", {}, [](const std::vector<std::any>&) -> std::any {
    return 6 * 6;
}};

auto ref = rt.submit(t);
rt.wait(ref);

int result = std::any_cast<int>(rt.get(ref)); // 36
rt.shutdown();
```

---

### Distributed Layer (`src/distributed/`)

#### NodeRuntime (`node_runtime.h/cpp`)

Represents a single physical (or logical) node in the cluster. Wraps a `Local::Runtime` and will eventually host an RPC server.

- Auto-generates a unique `node_id` on construction
- Calls `register_with_cluster()` on `start()` (currently logs; RPC hook is stubbed for Phase 2)
- Configurable worker count and port number

#### NodeRegistry (`cluster/node_registry.h/cpp`)

Maintains the live set of nodes known to the cluster.

| Method | Behaviour |
|---|---|
| `register_node(info)` | Add or update a node |
| `remove_node(id)` | Mark a node dead |
| `heartbeat(id)` | Update liveness (future: TTL-based eviction) |
| `pick_node()` | Round-robin node selection |

`NodeInfo` carries `node_id`, `address` (`host:port`), `available_workers`, and an `alive` flag.

#### ClusterScheduler (`cluster/cluster_scheduler.h/cpp`)

Cluster-wide counterpart to the local `Scheduler`.

- Accepts tasks via `submit(task)`
- Gates dispatch on dep readiness (checks internal `global_objects_` map)
- **Plan-then-Dispatch Model**: Decouples dependency resolution from network I/O to prevent deadlocks and optimize throughput.
- **Speculative Execution**: Monitors task latency and dispatches "clones" to bypass node stragglers.
- **Integrity Verification**: Verifies SHA-256 hashes of results against expected values to prevent the "Poisonous Worker" problem.

```
ClusterScheduler::submit(task)
    └── schedule()
            ├── deps_ready_?  [check object_locations_]
            ├── registry_.pick_node()
            ├── client_.submit_task(node_id, task)
            └── on_object_created(task.id, node_id)
```

#### NodeClient (`rpc/node_client.h`)

Abstract interface for sending tasks to a node.

```cpp
class NodeClient {
public:
    virtual ObjectRef submit_task(const std::string& node_id, Task task) = 0;
};
```

#### InProcessNodeClient (`rpc/inprocess_node_client.h`)

Concrete `NodeClient` for testing and single-binary cluster simulation. Holds raw pointers to `NodeRuntime` instances and routes calls directly — no network involved.

---

## Usage Examples

### Multi-node cluster (gRPC)

Start the cluster head server:

```bash
./head 50050
```

Start one or more worker nodes in separate terminals:

```bash
./node 50050 6001 node-1
./node 50050 6002 node-2
```

Submit test tasks to the cluster:

```bash
./submit_test 50050
```

### Single-process Debug (Local Runtime)

```cpp

    NodeRuntime n1(2, 5001);
    NodeRuntime n2(2, 5002);

    n1.start();
    n2.start();

    NodeRegistry registry;
    registry.register_node({"node-1", "localhost:5001", 2, true});
    registry.register_node({"node-2", "localhost:5002", 2, true});

    InProcessNodeClient client;
    client.add_node("node-1", &n1);
    client.add_node("node-2", &n2);

    ClusterScheduler cluster(registry, client);

    // Task A
    orion::Task t1{
        "A",
        {},
        [](const std::vector<std::any>&) -> std::any { return 10; }
    };

    // Task B depends on A
    orion::Task t2{
        "B",
        {orion::ObjectRef{"A"}},
        [](std::vector<std::any> args) -> std::any {
            int a = std::any_cast<int>(args[0]);
            return a + 32;
        }
    };

    cluster.submit(t1);
    cluster.submit(t2);

    // In v0.2 we assumed locations at dispatch time;
    // real completion tracking comes next (heartbeats/object reports).
    // For now you can just sleep or block at node-local store if you expose it.

    std::cout << "Cluster scheduled tasks.\n";

    n1.stop();
    n2.stop();
```

### Multi-node cluster (in-process simulation)

```cpp
using namespace orion::distributed;

NodeRuntime n1(2, 5001), n2(2, 5002);
n1.start(); n2.start();

NodeRegistry registry;
registry.register_node({"node-1", "localhost:5001", 2, true});
registry.register_node({"node-2", "localhost:5002", 2, true});

InProcessNodeClient client;
client.add_node("node-1", &n1);
client.add_node("node-2", &n2);

ClusterScheduler cluster(registry, client);

// Task A: no deps
orion::Task t1{"A", {}, [](const std::vector<std::any>&) -> std::any { return 10; }};

// Task B: depends on A
orion::Task t2{"B", {orion::ObjectRef{"A"}},
    [](std::vector<std::any> args) -> std::any {
        return std::any_cast<int>(args[0]) + 32; // 42
    }};

cluster.submit(t1);
cluster.submit(t2);

n1.stop(); n2.stop();
```

---

## Benchmarks: Scaling Redis Core

Orion was validated against the **Redis 7.x** codebase (114 core tasks). By implementing a **Global Context Uplift** strategy (multi-threaded pre-provisioning of the entire source tree to worker sandboxes), we achieved near-linear scaling.

| Configuration | Tasks | Total Time | Speedup | Efficiency |
| :--- | :---: | :---: | :---: | :---: |
| **1-Node Baseline** | 114 | 10.74 seconds | 1.0x | 100% |
| **6-Node Cluster** | 114 | **1.92 seconds** | **5.6x** | **93%** |

### Key Scalability Features:
- **Global Context Uplift**: Eliminates "header not found" errors by ensuring worker sandboxes are identical to the project root.
- **gRPC Multiplexing**: Persistent channels prevent port exhaustion under high task concurrency (50+ tasks/sec).
- **DAG Extraction**: Python-based automation for mapping arbitrary C/C++ projects into Orion distributed graphs.

---


## Building

Requires **C++23** and a POSIX-compatible system (pthreads).

```bash
# Build with Make (recommended)
make

# Clean
make clean
```

The Makefile uses `clang++` with `-std=c++23 -O2 -pthread`. To use GCC:

```bash
CXX=g++ make
```

---

## Design Principles

1. **Dataflow semantics** — tasks run when their inputs exist, not when the caller says so
2. **Layered design** — core engine is network-agnostic; distribution is opt-in
3. **Thread safety throughout** — mutexes + condition variables at every shared boundary
4. **Pluggable transport** — `NodeClient` abstraction decouples scheduling from RPC implementation
5. **Test-friendly** — `InProcessNodeClient` lets you run a full cluster in a single binary

---

#### Phase 5 — Orion V2: Production-Grade Reliability (Current)

Orion V2 marks the transition from a research prototype to a production-ready build engine. The core architectural change is the move to **Immutable Content-Addressing** and **Raft-based Consensus**.

- **Raft High Availability**: The `ClusterHead` is now a 3-replica state machine. Using Raft, the head maintains a linearized log of every task submission and metadata update, ensuring no progress is lost on leader failover.
- **Merkle Action Cache**: Tasks are no longer identified by random IDs. We compute a `action_hash = SHA-256(function_name || args || sorted(input_hashes))`. This "fingerprint" allows for instant cache recovery; if the hash exists in the `ActionCache`, we skip execution entirely.
- **P2P Distributed CAS**: Worker nodes no longer depend on the head node for dependency files. They fetch blobs directly from the producing node's `CasStore` via gRPC streaming, eliminating the head's network bottleneck.

#### Phase 1 — Core Engine & Local Runtime
- **The Issue**: Sequential build scripts fail to utilize multi-core hardware and manual dependency management is fragile.
- **The Solution**: **Dataflow DAG Scheduler**. Built a thread-safe task engine that automatically resolves dependencies and dispatches ready tasks to a worker pool.
- [x] Core task execution engine (workers, scheduler, object store)
- [x] Local `Runtime` façade
- [x] `NodeRuntime` (per-node wrapper with lifecycle management)
- [x] `NodeRegistry` (cluster membership, round-robin selection, heartbeat stub)
- [x] `ClusterScheduler` (cross-node dependency tracking and dispatch)
- [x] `NodeClient` abstraction + `InProcessNodeClient` for in-process testing
- [x] Multi-node dependency-chaining demo in `main.cpp`
- [x] **Real RPC transport using gRPC** (`head`, `node`, `submit_test` executables)

#### Phase 2 — gRPC Cluster & Fault Tolerance
- [x] Real RPC transport (gRPC) replacing `InProcessNodeClient`
- [x] Node-reported object location confirmations (ReportObjectCreated)
- [x] Heartbeat-based node liveness (TTL eviction + auto-requeue on dead-node reclaim, Phase 4.6)
- [x] Task failure handling and retry (dispatch rollback + bounded `ReportObjectCreated` retries + dep-wait timeout + in-flight hard timeout) — see Phase 4.5 hardening pass
- [x] Task failure handling with fully configurable policies (per-task retry budgets, jittered backoff, DLQ) — Phase 4.6
- [x] Speculative cancellation (CancelTask RPC + worker-side abort flag) — Phase 4.6
- [ ] Work-stealing across nodes

#### Phase 3 — Distributed Data Computation (V2)
- [x] Cross-process object serialization (Protobuf wire format)
- [x] **Distributed CAS** (shared storage + gRPC fetch_blob)
- [x] Streaming / chunked object support for large datasets

#### Phase 3.5 — Action Cache (V2)
- [x] **Merkle Action Hashing**: Command + Input Files + Toolchain hashing
- [x] **Global Action Cache**: Skip execution on hit
- [x] Worker-side `GetObject` streaming from CAS

#### Phase 6 — Global Control Store & High Availability (V2)
- [x] **Raft Consensus**: 3-replica state-machine for head node metadata
- [x] Persistent GCS with backing store (PV-based Raft logs)
- [x] Pub/sub object-ready notifications via state-machine replication

#### Phase 7 — Dashboard & Observability
- **The Issue**: Distributed builds are "black boxes." Pinpointing which node failed or why a specific module is slow requires manual log-diving across dozens of machines.
- **Production Solution**: **Distributed Tracing & Heatmaps**. Integrate OpenTelemetry to visualize DAG execution, identify "hotspot" nodes, and analyze critical-path latency in real-time.
- [ ] REST API exposing cluster state (nodes, tasks, object locations)
- [ ] Web dashboard: live task graph visualisation
- [ ] Distributed tracing (task lineage)


#### Phase 7 — Dashboard & Observability
- **The Issue**: Distributed builds are "black boxes." Pinpointing which node failed or why a specific module is slow requires manual log-diving across dozens of machines.
- **Production Solution**: **Distributed Tracing & Heatmaps**. Integrate OpenTelemetry to visualize DAG execution, identify "hotspot" nodes, and analyze critical-path latency in real-time.
- [ ] REST API exposing cluster state (nodes, tasks, object locations)
- [ ] Web dashboard: live task graph visualisation
- [ ] Distributed tracing (task lineage)

---

## License

MIT License

## Author

Samarth Mahendra
