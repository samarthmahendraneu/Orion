# Orion

A multi-threaded, dependency-aware task execution framework in C++23 — designed to scale from a single-machine runtime to a distributed cluster of worker nodes.

## Overview

Orion models computation as a **dataflow graph**: tasks declare their inputs as `ObjectRef` dependencies, and the scheduler dispatches them to workers only when all dependencies are satisfied. The framework is layered into three tiers:

| Tier | What it does |
|---|---|
| **Core** | Tasks, workers, scheduler, object store — the high-performance engine |
| **Local** | `Runtime` — a clean façade over the core for single-process use |
| **Distributed** | `NodeRuntime`, `ClusterScheduler`, `NodeRegistry`, `NodeClient` — gRPC-based cluster orchestration |
| **Hardened** | **Speculative Execution** (Straggler Mitigation) & **SHA-256 Integrity Verification** |

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

## Current Status

#### Phase 4.5 — Reliability Hardening Pass (this revision)

A post-audit pass closed five latent data-loss / liveness bugs. These are documented in detail in `challenges.md` (items 10–14) and touch only `src/`:

- **`ReportObjectCreated` no longer silently drops completions.** The worker now sets an RPC deadline and retries the "call-home" up to 5 times with exponential backoff; only then does it log a `CRITICAL` line. Previously the `grpc::Status` was discarded, so a lost completion RPC would permanently hang every downstream task.  *(File: `src/distributed/node_runtime.cpp`.)*
- **`GrpcNodeClient::submit_task` stops pretending to succeed.** The `NodeClient` interface now returns `bool`; a null stub, failed RPC, or server rejection surfaces the failure to the `ClusterScheduler`, which rolls back the in-flight entry and re-queues the task on another node. The cached stub is also dropped on transport failure so the next retry re-opens the channel.  *(Files: `src/distributed/rpc/node_client.h`, `grpc_node_client.h`, `inprocess_node_client.h`.)*
- **`in_flight_` map can no longer leak.** Stale entries whose node died (or whose completion RPC was lost forever) are now reaped by a hard timeout sweep in the background monitor thread. Rolled-back dispatches are also removed immediately.  *(File: `src/distributed/cluster/cluster_scheduler.cpp`.)*
- **Dependency-wait deadlock eliminated.** Every task has a `submit_time_`; if its deps haven't materialised within `dep_timeout_`, the task is marked as permanently failed. A `failed_objects_` set propagates the failure down the DAG so the entire subgraph fails fast instead of waiting forever.  *(Same file.)*
- **SHA-256 verification is now actually enforced.** The first completer's hash is recorded as the canonical hash; every speculative clone's hash is compared against it, and a mismatch produces a loud `!!! INTEGRITY MISMATCH !!!` log without overwriting the trusted value.  *(Same file, and `head_main.cpp` already passes the hash through.)*

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

### In Progress / Planned

#### Phase 2 — gRPC Cluster & Fault Tolerance
- **The Issue**: Local resources (CPU/RAM) are a hard ceiling for massive builds. Scaling requires moving from shared-memory threads to independent networked processes.
- **The Solution**: **gRPC Orchestration**. Implemented a Head-to-Worker RPC layer with a centralized `NodeRegistry` and `ReportObjectCreated` callbacks, enabling horizontal scaling.
- [x] Real RPC transport (gRPC) replacing `InProcessNodeClient`
- [x] Node-reported object location confirmations (ReportObjectCreated)
- [x] Heartbeat-based node liveness 
- [x] Task failure handling and retry (dispatch rollback + bounded `ReportObjectCreated` retries + dep-wait timeout + in-flight hard timeout) — see Phase 4.5 hardening pass
- [ ] Task failure handling with fully configurable policies (per-task retry budgets, jittered backoff, DLQ)
- [ ] Work-stealing across nodes

#### Phase 3 — Ad-hoc Distributed Data Computation
- **The Issue**: The Head is a central bandwidth bottleneck. All task outputs flow from Worker -> Head, and all inputs flow Head -> Worker. For large binaries, the Head's network interface would saturate.
- **Production Solution**: Implement **P2P CAS (Content Addressable Storage)**. Use a pull-based fetching model (similar to Ray Plasma) where workers fetch dependencies directly from the producing node.
- [ ] Cross-process object serialization (replace `std::any` with a wire format)
- [ ] Distributed object store (shared-memory + TCP pull)
- [ ] Streaming / chunked object support for large datasets

#### Phase 4 — Security & Reliability (Hardening)
- **The Issue**: Remote workers are "black boxes" that could return corrupted artifacts. Additionally, single slow nodes ("stragglers") can bottleneck the entire build.
- **The Solution**: **Speculative Execution & SHA-256 Hashing**. Integrated cryptographic verification for all artifacts and a "racing" mechanism to bypass node-level variance.
- [x] **Speculative Execution**: Heuristic-based straggler detection and task cloning
- [x] **Poisonous Worker Protection**: SHA-256 content-based integrity verification
- [x] **Concurrency Hardening**: "Plan-then-Dispatch" scheduler refactor for deadlock-free orchestration
- [ ] Support for recursive and speculative task patterns in core
- [ ] `ObjectRef` as a first-class future passable between tasks at runtime
- [ ] Group / barrier synchronisation primitives

#### Phase 5 — GPU & Heterogeneous Scheduling
- **The Issue**: Current scheduler assumes all nodes are identical. Scheduling a GPU-heavy task (like a Metal shader) on a CPU-only node leads to immediate failure or massive latency.
- **Production Solution**: Implement **Resource-Aware Scheduling**. Nodes heartbeat a telemetry vector (CPU/RAM/GPU), and the scheduler performs constraint-based matching (e.g., "Schedule only on M-series GPU").
- [ ] Resource annotations on `Task` (CPU cores, GPU count, memory)
- [ ] GPU-aware node selection in `ClusterScheduler`
- [ ] Mixed CPU + GPU pipeline scheduling

#### Phase 6 — Global Control Store (GCS) & High Availability
- **The Issue**: The `ClusterHead` is a Single Point of Failure (SPOF) and state is in-memory. If the Head process crashes, the entire build DAG and progress are lost.
- **Production Solution**: Implement **Raft-based Consensus**. Use a persistent GCS (like Etcd) to mirror cluster state and enable instant Leader election/failover for the Head node.
- [ ] Centralized GCS process for cluster-wide state (node registry, object table)
- [ ] Fault-tolerant GCS with persistent backing store
- [ ] Pub/sub object-ready notifications

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
