#include "src/build_engine/builder.h"
#include "src/distributed/node_runtime.h"
#include "src/distributed/cluster/node_registry.h"
#include "src/distributed/cluster/cluster_scheduler.h"
#include "src/distributed/rpc/inprocess_node_client.h"
#include <fstream>
#include <iostream>
#include <thread>
#include <chrono>

using namespace orion::distributed;

int main() {
    std::ofstream out_main("cluster_main.cpp");
    out_main << "#include <iostream>\n"
             << "void print_msg();\n"
             << "int main() { print_msg(); return 0; }\n";
    out_main.close();

    std::ofstream out_utils("cluster_utils.cpp");
    out_utils << "#include <iostream>\n"
              << "void print_msg() { std::cout << \"Hello from Orion Distributed Build Engine!\\n\"; }\n";
    out_utils.close();

    // Setup Orion Cluster
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

    orion::build_engine::BuildTarget target{
        "cluster_app", 
        {"cluster_main.cpp", "cluster_utils.cpp"}, 
        "clang++", 
        "-std=c++23 -O2"
    };

    orion::build_engine::Builder<ClusterScheduler> builder(cluster);

    std::cout << "Starting clustered build of target: " << target.name << "\n";
    orion::ObjectRef final_ref = builder.build(target);

    // Sleep to let cluster nodes process tasks
    std::this_thread::sleep_for(std::chrono::seconds(2));

    std::cout << "\nExecuting cluster_app:\n";
    std::cout << "--------------------------------------\n";
    std::system("./cluster_app");
    std::cout << "--------------------------------------\n";

    n1.stop();
    n2.stop();

    return 0;
}
