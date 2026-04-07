#include "src/build_engine/builder.h"
#include "src/distributed/node_runtime.h"
#include "src/distributed/cluster/node_registry.h"
#include "src/distributed/cluster/cluster_scheduler.h"
#include "src/distributed/rpc/inprocess_node_client.h"
#include "src/core/context.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>

using namespace orion::distributed;

int main() {
    const int NUM_TASKS = 20;
    const int SLEEP_MS = 100;

    std::cout << "=== Orion Framework Benchmark ===\n";
    std::cout << "Simulating " << NUM_TASKS << " compilation tasks, each taking " << SLEEP_MS << "ms.\n\n";

    // ------------------------------------------------------------------------------------------
    // 1. Sequential Execution
    // ------------------------------------------------------------------------------------------
    std::cout << "[Sequential] Starting sequential execution...\n";
    auto seq_start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < NUM_TASKS; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(SLEEP_MS));
    }
    
    auto seq_end = std::chrono::high_resolution_clock::now();
    double seq_dur = std::chrono::duration<double, std::milli>(seq_end - seq_start).count();
    std::cout << "[Sequential] Completed in " << seq_dur << " ms.\n\n";

    // ------------------------------------------------------------------------------------------
    // 2. Distributed / Parallel Execution (2 nodes, 4 workers total)
    // ------------------------------------------------------------------------------------------
    std::cout << "[Distributed] Setting up cluster...\n";
    NodeRuntime n1(2, 5001, "", "node-alpha", "");
    NodeRuntime n2(2, 5002, "", "node-beta", "");

    n1.start();
    n2.start();

    NodeRegistry registry;
    registry.register_node({"node-alpha", "localhost:5001", 2, true});
    registry.register_node({"node-beta", "localhost:5002", 2, true});

    InProcessNodeClient client;
    client.add_node("node-alpha", &n1);
    client.add_node("node-beta", &n2);

    ClusterScheduler cluster(registry, client);

    n1.local_runtime().store().set_global_context(&cluster);
    n2.local_runtime().store().set_global_context(&cluster);

    std::cout << "[Distributed] Submitting tasks...\n";
    auto dist_start = std::chrono::high_resolution_clock::now();
    
    std::vector<orion::ObjectRef> refs;
    for (int i = 0; i < NUM_TASKS; ++i) {
        orion::Task t{
            "task_" + std::to_string(i),
            {},
            [SLEEP_MS](const std::vector<std::any>&) -> std::any {
                std::this_thread::sleep_for(std::chrono::milliseconds(SLEEP_MS));
                return 0; // Success
            }
        };
        refs.push_back(cluster.submit(t));
    }

    // Task to link/gather all
    orion::Task link_task{
        "link_all",
        refs,
        [](const std::vector<std::any>&) -> std::any {
            return 0;
        }
    };
    auto link_ref = cluster.submit(link_task);

    // Naive busy wait for completion because Orion v0.2 relies on blocking getters or timeouts 
    // for cross-node right now. We'll poll local runtimes just in case, but link task depends on all.
    while (true) {
        // Sleep a bit and check if all tasks are done or simply wait 
        // We know link_task will only run once everything is done. The simplest way here is to 
        // wait for the scheduler queues to drain but let's just do a pragmatic sleep loop since 
        // cluster scheduler object lookup isn't exposed yet for in-process client polling nicely.
        // Actually since we gave them a global_context, we can try getting from store.
        auto val = n1.local_runtime().store().get(link_ref.id);
        if (val) break;
        val = n2.local_runtime().store().get(link_ref.id);
        if (val) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto dist_end = std::chrono::high_resolution_clock::now();
    double dist_dur = std::chrono::duration<double, std::milli>(dist_end - dist_start).count();
    std::cout << "[Distributed] Completed in " << dist_dur << " ms.\n\n";

    n1.stop();
    n2.stop();

    // ------------------------------------------------------------------------------------------
    // 3. Results Calculation
    // ------------------------------------------------------------------------------------------
    double speedup = seq_dur / dist_dur;
    double reduction = ((seq_dur - dist_dur) / seq_dur) * 100.0;
    
    std::cout << "=== Benchmark Results ===\n";
    std::cout << "Speedup: " << speedup << "x\n";
    std::cout << "Time Reduction: " << reduction << "%\n";

    return 0;
}
