#include "src/build_engine/builder.h"
#include "src/distributed/node_runtime.h"
#include "src/distributed/cluster/node_registry.h"
#include "src/distributed/cluster/cluster_scheduler.h"
#include "src/distributed/rpc/inprocess_node_client.h"
#include "src/core/context.h"
#include <fstream>
#include <iostream>
#include <thread>
#include <chrono>

using namespace orion::distributed;

int main() {
    // ------------------------------------------------------------------------------------------
    // 1. We'll set up a Cluster just like before.
    // ------------------------------------------------------------------------------------------
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

    // Supply global context to local runtimes for cross-node fetching
    n1.local_runtime().store().set_global_context(&cluster);
    n2.local_runtime().store().set_global_context(&cluster);

    // ------------------------------------------------------------------------------------------
    // 2. We'll manually construct a dependency graph showing parallel compilation -> linking
    // ------------------------------------------------------------------------------------------
    std::cout << "\n=== Starting Independent Parallel Build Task Graph ===\n\n";

    // Setup: write source files to disk immediately
    std::ofstream h_out("math_lib.h");
    h_out << "#pragma once\nint add(int a, int b);\n";
    h_out.close();

    std::ofstream cpp_out("math_lib.cpp");
    cpp_out << "#include \"math_lib.h\"\nint add(int a, int b) { return a + b; }\n";
    cpp_out.close();

    std::ofstream main_out("main_complex.cpp");
    main_out << "#include <iostream>\n#include \"math_lib.h\"\n"
             << "int main() { std::cout << \"Distributed App Result: \" << add(10, 32) << \"\\n\"; return 0; }\n";
    main_out.close();

    // Task 1: Compile math_lib.cpp (No dependencies, runs instantly on any node)
    orion::Task compile_math{
        "compile_math",
        {},
        [](const std::vector<std::any>&) -> std::any {
            std::cout << "[Compile] Node \033[1;32m[" << orion::get_current_node_id() << "]\033[0m compiling math_lib.cpp\n";
            return std::system("clang++ -std=c++23 -O2 -c math_lib.cpp -o math_lib.o");
        }
    };

    auto math_ref = cluster.submit(compile_math);

    // Task 2: Compile main_complex.cpp (No dependencies, runs instantly on any node)
    orion::Task compile_main{
        "compile_main",
        {},
        [](const std::vector<std::any>&) -> std::any {
            std::cout << "[Compile] Node \033[1;32m[" << orion::get_current_node_id() << "]\033[0m compiling main_complex.cpp\n";
            return std::system("clang++ -std=c++23 -O2 -c main_complex.cpp -o main_complex.o");
        }
    };

    auto main_ref = cluster.submit(compile_main);

    // Task 4: Link everything (Depends on both compilations!)
    orion::Task link_app{
        "link_app",
        {math_ref, main_ref},
        [](const std::vector<std::any>& args) -> std::any {
            if (std::any_cast<int>(args[0]) != 0 || std::any_cast<int>(args[1]) != 0) return -1;
            std::cout << "[Linker]  Node \033[1;34m[" << orion::get_current_node_id() << "]\033[0m linking complex_app\n";
            return std::system("clang++ math_lib.o main_complex.o -o complex_app");
        }
    };

    cluster.submit(link_app);

    // Sleep to allow tasks to run across nodes
    std::this_thread::sleep_for(std::chrono::seconds(2));

    std::cout << "\n--------------------------------------\n";
    std::system("./complex_app");
    std::cout << "--------------------------------------\n";

    n1.stop();
    n2.stop();

    return 0;
}
