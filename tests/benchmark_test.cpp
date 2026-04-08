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
#include <fstream>
#include <cstdlib>

using namespace orion::distributed;

int main() {
    const int NUM_TASKS = 32; // Increased tasks to properly saturate 8 workers

    std::cout << "=== Orion Framework Benchmark (Real C++ Compilation Workload) ===\n";
    std::cout << "Generating " << NUM_TASKS << " unique C++ source files...\n";

    // 1. Generate real C++ files
    for (int i = 0; i < NUM_TASKS; ++i) {
        std::ofstream out("bench_src_" + std::to_string(i) + ".cpp");
        out << "#include <vector>\n"
            << "#include <algorithm>\n"
            << "int bench_func_" << i << "(int val) {\n"
            << "    std::vector<int> v(100000, val);\n" // Force some compilation work (templates)
            << "    std::sort(v.begin(), v.end());\n"
            << "    return v[0] + " << i << ";\n"
            << "}\n";
    }

    std::cout << "\n[Sequential] Starting sequential compilation...\n";
    auto seq_start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < NUM_TASKS; ++i) {
        std::string cmd = "clang++ -std=c++23 -O2 -c bench_src_" + std::to_string(i) + ".cpp -o seq_obj_" + std::to_string(i) + ".o";
        if (std::system(cmd.c_str()) != 0) {
            std::cerr << "Compilation failed for task " << i << "\n";
        }
    }
    
    auto seq_end = std::chrono::high_resolution_clock::now();
    double seq_dur = std::chrono::duration<double, std::milli>(seq_end - seq_start).count();
    std::cout << "[Sequential] Completed in " << seq_dur << " ms.\n\n";

    // ------------------------------------------------------------------------------------------
    // Distributed / Parallel Execution (4 nodes, 8 workers total)
    // ------------------------------------------------------------------------------------------
    std::cout << "[Distributed] Setting up cluster (4 nodes)...\n";
    NodeRuntime n1(2, 5001, "", "node-alpha", "");
    NodeRuntime n2(2, 5002, "", "node-beta", "");
    NodeRuntime n3(2, 5003, "", "node-gamma", "");
    NodeRuntime n4(2, 5004, "", "node-delta", "");

    n1.start();
    n2.start();
    n3.start();
    n4.start();

    NodeRegistry registry;
    registry.register_node({"node-alpha", "localhost:5001", 2, true});
    registry.register_node({"node-beta", "localhost:5002", 2, true});
    registry.register_node({"node-gamma", "localhost:5003", 2, true});
    registry.register_node({"node-delta", "localhost:5004", 2, true});

    InProcessNodeClient client;
    client.add_node("node-alpha", &n1);
    client.add_node("node-beta", &n2);
    client.add_node("node-gamma", &n3);
    client.add_node("node-delta", &n4);

    ClusterScheduler cluster(registry, client);

    n1.local_runtime().store().set_global_context(&cluster);
    n2.local_runtime().store().set_global_context(&cluster);
    n3.local_runtime().store().set_global_context(&cluster);
    n4.local_runtime().store().set_global_context(&cluster);

    std::cout << "[Distributed] Submitting compilation tasks...\n";
    auto dist_start = std::chrono::high_resolution_clock::now();
    
    std::vector<orion::ObjectRef> refs;
    for (int i = 0; i < NUM_TASKS; ++i) {
        orion::Task t{
            "task_compile_" + std::to_string(i),
            {},
            [i](const std::vector<std::any>&) -> std::any {
                std::string cmd = "clang++ -std=c++23 -O2 -c bench_src_" + std::to_string(i) + ".cpp -o dist_obj_" + std::to_string(i) + ".o";
                int ret = std::system(cmd.c_str());
                return (ret == 0) ? 1 : 0; // Return 1 for success, 0 for failure
            }
        };
        refs.push_back(cluster.submit(t));
    }

    // Task to link everything
    orion::Task link_task{
        "link_all",
        refs,
        [](const std::vector<std::any>& args) -> std::any {
            int success_count = 0;
            for (const auto& a : args) {
                success_count += std::any_cast<int>(a);
            }
            return success_count;
        }
    };
    auto link_ref = cluster.submit(link_task);

    while (true) {
        auto val = n1.local_runtime().store().get(link_ref.id);
        if (val) {
            std::cout << "[Validation] Link task verified " << std::any_cast<int>(val.value()) << " out of " << NUM_TASKS << " compilations succeeded!\n";
            break;
        }
        val = n2.local_runtime().store().get(link_ref.id);
        if (val) {
            std::cout << "[Validation] Link task verified " << std::any_cast<int>(val.value()) << " out of " << NUM_TASKS << " compilations succeeded!\n";
            break;
        }
        val = n3.local_runtime().store().get(link_ref.id);
        if (val) {
            std::cout << "[Validation] Link task verified " << std::any_cast<int>(val.value()) << " out of " << NUM_TASKS << " compilations succeeded!\n";
            break;
        }
        val = n4.local_runtime().store().get(link_ref.id);
        if (val) {
            std::cout << "[Validation] Link task verified " << std::any_cast<int>(val.value()) << " out of " << NUM_TASKS << " compilations succeeded!\n";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    auto dist_end = std::chrono::high_resolution_clock::now();
    double dist_dur = std::chrono::duration<double, std::milli>(dist_end - dist_start).count();
    std::cout << "[Distributed] Completed in " << dist_dur << " ms.\n\n";

    n1.stop();
    n2.stop();
    n3.stop();
    n4.stop();

    // Cleanup generated files
    std::cout << "Cleaning up generated files...\n";
    std::system("rm bench_src_*.cpp seq_obj_*.o dist_obj_*.o 2>/dev/null");

    // ------------------------------------------------------------------------------------------
    // Results Calculation
    // ------------------------------------------------------------------------------------------
    double speedup = seq_dur / dist_dur;
    double reduction = ((seq_dur - dist_dur) / seq_dur) * 100.0;
    
    std::cout << "=== REAL C++ COMPILATION BENCHMARK RESULTS ===\n";
    std::cout << "Speedup: " << speedup << "x\n";
    std::cout << "Time Reduction: " << reduction << "%\n";

    return 0;
}
