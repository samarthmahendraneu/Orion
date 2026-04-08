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
    std::string port = (argc > 1) ? argv[1] : "50050";
    std::string target = "localhost:" + port;

    auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
    auto stub = orion::ClusterHead::NewStub(channel);

    const int NUM_TASKS = 64;
    std::cout << "=== Orion Framework Benchmark (70% Target Run) ===\n";
    std::cout << "Generating " << NUM_TASKS << " utility modules...\n";

    std::system("rm bench_util_*.h bench_util_*.cpp bench_main.cpp seq_obj_*.o dist_obj_*.o bench_exec_* 2>/dev/null");

    for (int i = 0; i < NUM_TASKS; ++i) {
        std::ofstream h("bench_util_" + std::to_string(i) + ".h");
        h << "long long bench_func_" << i << "(long long val);\n";

        std::ofstream cpp("bench_util_" + std::to_string(i) + ".cpp");
        cpp << "#include \"bench_util_" << i << ".h\"\n\n"
            << "constexpr long long heavy_compute(long long base) {\n"
            << "    long long sum = base;\n"
            << "    for (long long j = 0; j < 2000000; ++j) {\n"
            << "        sum += (j * j) % 1000000007;\n"
            << "        sum ^= (sum >> 3);\n"
            << "    }\n"
            << "    return sum;\n"
            << "}\n\n"
            << "long long bench_func_" << i << "(long long val) {\n"
            << "    constexpr long long result = heavy_compute(" << i << ");\n"
            << "    return result + val;\n"
            << "}\n";
    }

    std::ofstream main_cpp("bench_main.cpp");
    for (int i = 0; i < NUM_TASKS; ++i) main_cpp << "#include \"bench_util_" << i << ".h\"\n";
    main_cpp << "int main() {\n    long long sum = 0;\n";
    for (int i = 0; i < NUM_TASKS; ++i) main_cpp << "    sum += bench_func_" << i << "(0);\n";
    main_cpp << "    return sum % 1000;\n}\n";
    main_cpp.close();

    // Sequential
    std::cout << "\n[Sequential] Running baseline...\n";
    auto seq_start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < NUM_TASKS; ++i) {
        std::string cmd = "clang++ -std=c++23 -O2 -fconstexpr-steps=10000000 -c bench_util_" + std::to_string(i) + ".cpp -o seq_obj_util_" + std::to_string(i) + ".o";
        std::system(cmd.c_str());
    }
    std::system("clang++ -std=c++23 -O2 -fconstexpr-steps=10000000 -c bench_main.cpp -o seq_obj_main.o");
    std::system("clang++ -std=c++23 -O2 seq_obj_util_*.o seq_obj_main.o -o bench_exec_seq");
    auto seq_end = std::chrono::high_resolution_clock::now();
    double seq_dur = std::chrono::duration<double, std::milli>(seq_end - seq_start).count();
    std::cout << "[Sequential] Completed in " << seq_dur << " ms.\n\n";

    // Distributed
    auto pack_int = [](int v) -> std::string {
        std::string b(4, '\0');
        std::memcpy(b.data(), &v, 4);
        return b;
    };

    std::cout << "[Distributed] Submitting tasks...\n";
    auto dist_start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < NUM_TASKS; ++i) {
        orion::TaskRequest req;
        req.set_task_id("task_util_" + std::to_string(i));
        req.set_function_name("compile_util");
        req.add_args(pack_int(i));
        orion::TaskReply reply;
        grpc::ClientContext ctx;
        stub->SubmitTask(&ctx, req, &reply);
    }
    {
        orion::TaskRequest req;
        req.set_task_id("task_main");
        req.set_function_name("compile_main");
        req.add_args(pack_int(0));
        orion::TaskReply reply;
        grpc::ClientContext ctx;
        stub->SubmitTask(&ctx, req, &reply);
    }

    while (true) {
        int ready = 0;
        for (int i = 0; i < NUM_TASKS; ++i) {
            std::ifstream f("dist_obj_util_" + std::to_string(i) + ".o");
            if (f.good()) ready++;
        }
        std::ifstream fm("dist_obj_main.o");
        if (fm.good()) ready++;
        if (ready == NUM_TASKS + 1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    orion::TaskRequest lreq;
    lreq.set_task_id("task_link");
    lreq.set_function_name("link_exec");
    lreq.add_args(pack_int(0));
    orion::TaskReply lreply;
    grpc::ClientContext lctx;
    stub->SubmitTask(&lctx, lreq, &lreply);

    while (true) {
        std::ifstream f("bench_exec_dist");
        if (f.good()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    auto dist_end = std::chrono::high_resolution_clock::now();
    double dist_dur = std::chrono::duration<double, std::milli>(dist_end - dist_start).count();
    std::cout << "[Distributed] Completed in " << dist_dur << " ms.\n\n";

    double speedup = seq_dur / dist_dur;
    double reduction = ((seq_dur - dist_dur) / seq_dur) * 100.0;
    std::cout << "=== RESULTS ===\nSpeedup: " << speedup << "x\nReduction: " << reduction << "%\n";

    return 0;
}
