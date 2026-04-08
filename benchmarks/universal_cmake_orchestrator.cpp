#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <thread>

#include <grpcpp/grpcpp.h>
#include "distributed/generated/orion.grpc.pb.h"

namespace fs = std::filesystem;

struct BuildTask {
    std::string id;
    std::string command;
    std::vector<std::string> deps;
};

void run_distributed(orion::ClusterHead::Stub& stub, const std::vector<BuildTask>& tasks, const std::string& final_binary) {
    auto start = std::chrono::high_resolution_clock::now();

    for (const auto& bt : tasks) {
        orion::TaskRequest req;
        req.set_task_id(bt.id);
        req.set_function_name("shell_execute");
        req.add_args(bt.command);
        for (const auto& dep : bt.deps) {
            req.add_dep_ids(dep);
        }

        orion::TaskReply reply;
        grpc::ClientContext ctx;
        stub.SubmitTask(&ctx, req, &reply);
    }

    std::cout << "[Orion] Universal Deep & Wide DAG submitted. Waiting for " << final_binary << "...\n";
    while (!fs::exists(final_binary)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    auto end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start).count();
    std::cout << "[Orion] Distributed Build finished in " << ms << " ms.\n";
}

int main(int argc, char* argv[]) {
    std::string address = (argc > 1) ? argv[1] : "localhost:50050";
    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    auto stub = orion::ClusterHead::NewStub(channel);

    const int CHAINS = 4;
    const int DEPTH = 8;
    
    std::cout << "=== Orion Universal Build Orchestrator (Deep & Wide) ===\n";
    std::cout << "Target: 4 parallel chains of depth 8 (Total depth 10 stages)\n";

    std::vector<BuildTask> dag;
    std::system("rm -rf dist_build seq_build && mkdir -p dist_build seq_build");

    const std::string common_flags = "-std=c++23 -O2 -Iinput/sample_cmake_project/include -fconstexpr-steps=10000000";

    std::vector<std::string> all_module_ids;
    std::vector<std::string> tail_ids;

    // 1. Build the Deep Chains
    for (int c = 0; c < CHAINS; ++c) {
        std::string prev_id = "";
        for (int d = 0; d < DEPTH; ++d) {
            std::string id = "c" + std::to_string(c) + "_d" + std::to_string(d);
            std::string cmd = "clang++ " + common_flags + " -c input/sample_cmake_project/src/util_" + id + ".cpp -o dist_build/util_" + id + ".o";
            
            std::vector<std::string> deps;
            if (!prev_id.empty()) deps.push_back(prev_id); // 👈 The "Deep" part

            dag.push_back({id, cmd, deps});
            all_module_ids.push_back(id);
            prev_id = id;
        }
        tail_ids.push_back(prev_id);
    }
    
    // 2. Main app (depends on the tails of the chains)
    dag.push_back({"compile_main", "clang++ " + common_flags + " -c input/sample_cmake_project/src/main.cpp -o dist_build/main.o", tail_ids});
    all_module_ids.push_back("compile_main");
    
    // 3. Final Link (depends on everything)
    dag.push_back({"final_link", "clang++ dist_build/*.o -o dist_build/sample_app", all_module_ids});

    // 4. Run Sequential Baseline
    auto seq_start = std::chrono::high_resolution_clock::now();
    for(auto t : dag) {
        // Update command for seq dir
        size_t pos = 0;
        while((pos = t.command.find("dist_build/", pos)) != std::string::npos) {
            t.command.replace(pos, 11, "seq_build/");
            pos += 10;
        }
        std::system(t.command.c_str());
    }
    auto seq_end = std::chrono::high_resolution_clock::now();
    double seq_ms = std::chrono::duration<double, std::milli>(seq_end - seq_start).count();
    std::cout << "[Seq] Completed in " << seq_ms << " ms.\n";

    // 5. Run Distributed
    run_distributed(*stub, dag, "dist_build/sample_app");

    return 0;
}
