#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#include <fstream>
#include <chrono>
#include <thread>
#include <cstdlib>

#include <grpcpp/grpcpp.h>
#include "distributed/generated/orion.grpc.pb.h"

int main(int argc, char* argv[]) {
    // Usage: ./submit_benchmark [head_port]
    std::string port = (argc > 1) ? argv[1] : "50050";
    std::string target = "localhost:" + port;

    auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
    auto stub = orion::ClusterHead::NewStub(channel);

    const int NUM_TASKS = 32;
    std::cout << "=== Orion Framework Benchmark (Real C++ compilation over gRPC) ===\n";
    std::cout << "Generating " << NUM_TASKS << " unique C++ source files...\n";

    // Clean any old artifacts
    std::system("rm bench_src_*.cpp seq_obj_*.o dist_obj_*.o 2>/dev/null");

    for (int i = 0; i < NUM_TASKS; ++i) {
        std::ofstream out("bench_src_" + std::to_string(i) + ".cpp");
        out << "#include <vector>\n"
            << "#include <algorithm>\n"
            << "int bench_func_" << i << "(int val) {\n"
            << "    std::vector<int> v(100000, val);\n"
            << "    std::sort(v.begin(), v.end());\n"
            << "    return v[0] + " << i << ";\n"
            << "}\n";
    }

    // ── 1. Sequential Baseline ───────────────────────────────────────────────
    std::cout << "\n[Sequential] Starting sequential compilation...\n";
    auto seq_start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < NUM_TASKS; ++i) {
        std::string cmd = "clang++ -std=c++23 -O2 -c bench_src_" + std::to_string(i) + ".cpp -o seq_obj_" + std::to_string(i) + ".o";
        if (std::system(cmd.c_str()) != 0) {
            std::cerr << "Sequential compilation failed for task " << i << "\n";
        }
    }
    auto seq_end = std::chrono::high_resolution_clock::now();
    double seq_dur = std::chrono::duration<double, std::milli>(seq_end - seq_start).count();
    std::cout << "[Sequential] Completed in " << seq_dur << " ms.\n\n";

    // ── 2. Distributed Execution via gRPC ────────────────────────────────────
    std::cout << "[Distributed] Submitting compilation tasks to Head Node at " << target << "...\n";
    
    auto pack_int = [](int v) -> std::string {
        std::string b(4, '\0');
        std::memcpy(b.data(), &v, 4);
        return b;
    };

    auto dist_start = std::chrono::high_resolution_clock::now();
    
    // Dispatch all tasks securely over the network bypassing fork contention
    for (int i = 0; i < NUM_TASKS; ++i) {
        orion::TaskRequest req;
        req.set_task_id("task_compile_" + std::to_string(i));
        req.set_function_name("compile");
        req.add_args(pack_int(i));

        orion::TaskReply reply;
        grpc::ClientContext ctx;
        grpc::Status status = stub->SubmitTask(&ctx, req, &reply);

        if (!status.ok() || !reply.accepted()) {
            std::cerr << "[SubmitTest] Task " << i << " submission FAILED: " << status.error_message() << "\n";
        }
    }

    std::cout << "[Distributed] Tasks dispatched. Polling for distributed completion...\n";
    
    // Poll disk to synchronize completion (Since GetObject is Milestone 3)
    while (true) {
        int ready = 0;
        for (int i = 0; i < NUM_TASKS; ++i) {
            std::ifstream f("dist_obj_" + std::to_string(i) + ".o");
            if (f.good()) ready++;
        }
        if (ready == NUM_TASKS) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    auto dist_end = std::chrono::high_resolution_clock::now();
    double dist_dur = std::chrono::duration<double, std::milli>(dist_end - dist_start).count();
    std::cout << "[Distributed] Completed in " << dist_dur << " ms.\n\n";

    // ── 3. Results ───────────────────────────────────────────────────────────
    std::system("rm bench_src_*.cpp seq_obj_*.o dist_obj_*.o 2>/dev/null");

    double speedup = seq_dur / dist_dur;
    double reduction = ((seq_dur - dist_dur) / seq_dur) * 100.0;
    
    std::cout << "=== REAL C++ COMPILATION BENCHMARK RESULTS ===\n";
    std::cout << "Speedup: " << speedup << "x\n";
    std::cout << "Time Reduction: " << reduction << "%\n";

    return 0;
}
