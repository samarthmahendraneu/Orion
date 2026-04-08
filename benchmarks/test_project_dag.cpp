#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "src/distributed/generated/orion.grpc.pb.h"

namespace {

constexpr int kNumModules = 24;
constexpr int kConstexprIterations = 1200000;

namespace fs = std::filesystem;

std::string pack_int(int value) {
    std::string bytes(4, '\0');
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}

void write_file(const fs::path& path, const std::string& contents) {
    std::ofstream out(path);
    out << contents;
}

void generate_test_project() {
    fs::remove_all("test_project");
    fs::create_directories("test_project/include/test_project");
    fs::create_directories("test_project/src");
    fs::create_directories("test_project/build/local");
    fs::create_directories("test_project/build/distributed");

    write_file("test_project/include/test_project/common.h", R"(#pragma once
#include <array>
#include <cstdint>
#include <numeric>

namespace test_project {

template <std::size_t N>
constexpr std::array<std::uint64_t, N> seeded_values(std::uint64_t seed) {
    std::array<std::uint64_t, N> values{};
    for (std::size_t i = 0; i < N; ++i) {
        seed = (seed * 48271u + 1u) % 2147483647u;
        values[i] = seed ^ (seed >> 7);
    }
    return values;
}

template <std::size_t N>
constexpr std::uint64_t fold_values(const std::array<std::uint64_t, N>& values) {
    std::uint64_t acc = 0;
    for (std::size_t i = 0; i < N; ++i) {
        acc += values[i] * (i + 3);
        acc ^= (acc << 5) | (acc >> 2);
    }
    return acc;
}

}  // namespace test_project
)");

    for (int i = 0; i < kNumModules; ++i) {
        std::string header;
        header += "#pragma once\n";
        header += "#include <cstdint>\n";
        header += "std::uint64_t module_" + std::to_string(i) + "_value(std::uint64_t input);\n";
        write_file("test_project/include/test_project/module_" + std::to_string(i) + ".h", header);

        std::string source;
        source += "#include \"test_project/module_" + std::to_string(i) + ".h\"\n";
        source += "#include \"test_project/common.h\"\n";
        if (i > 0) {
            source += "#include \"test_project/module_" + std::to_string(i - 1) + ".h\"\n";
        }
        if (i > 1) {
            source += "#include \"test_project/module_" + std::to_string(i - 2) + ".h\"\n";
        }
        source += "\n";
        source += "namespace {\n";
        source += "constexpr std::uint64_t compile_time_value = [] {\n";
        source += "    auto values = test_project::seeded_values<" + std::to_string(kConstexprIterations / 3000) + ">(" + std::to_string(97 + i * 13) + "u);\n";
        source += "    std::uint64_t acc = test_project::fold_values(values);\n";
        source += "    for (int j = 0; j < " + std::to_string(kConstexprIterations) + "; ++j) {\n";
        source += "        acc += (static_cast<std::uint64_t>(j) * 2654435761ull) ^ (acc >> 3);\n";
        source += "        acc ^= (acc << 9);\n";
        source += "    }\n";
        source += "    return acc;\n";
        source += "}();\n";
        source += "}  // namespace\n\n";
        source += "std::uint64_t module_" + std::to_string(i) + "_value(std::uint64_t input) {\n";
        source += "    std::uint64_t result = compile_time_value + input + " + std::to_string(i) + "u;\n";
        if (i > 0) {
            source += "    result ^= module_" + std::to_string(i - 1) + "_value(input % 11u);\n";
        }
        if (i > 1) {
            source += "    result += module_" + std::to_string(i - 2) + "_value(input % 7u);\n";
        }
        source += "    return result;\n";
        source += "}\n";

        write_file("test_project/src/module_" + std::to_string(i) + ".cpp", source);
    }

    std::string main_source;
    for (int i = 0; i < kNumModules; ++i) {
        main_source += "#include \"test_project/module_" + std::to_string(i) + ".h\"\n";
    }
    main_source += "#include <cstdint>\n\n";
    main_source += "int main() {\n";
    main_source += "    std::uint64_t total = 0;\n";
    for (int i = 0; i < kNumModules; ++i) {
        main_source += "    total += module_" + std::to_string(i) + "_value(" + std::to_string(i + 1) + "u);\n";
    }
    main_source += "    return static_cast<int>(total % 251);\n";
    main_source += "}\n";
    write_file("test_project/src/main.cpp", main_source);

    write_file("test_project/README.md", R"(# test_project

Generated benchmark fixture for comparing Orion build throughput on a single node versus a 4-node cluster.
)");
}

void clean_local_outputs() {
    fs::create_directories("test_project/build/local");
    for (const auto& entry : fs::directory_iterator("test_project/build/local")) {
        fs::remove_all(entry.path());
    }
}

void clean_distributed_outputs() {
    fs::create_directories("test_project/build/distributed");
    for (const auto& entry : fs::directory_iterator("test_project/build/distributed")) {
        fs::remove_all(entry.path());
    }
}

bool run_command(const std::string& command) {
    return std::system(command.c_str()) == 0;
}

double run_local_build() {
    clean_local_outputs();

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kNumModules; ++i) {
        const std::string command =
            "clang++ -std=c++23 -O2 -fconstexpr-steps=4000000 "
            "-Itest_project/include "
            "-c test_project/src/module_" + std::to_string(i) + ".cpp "
            "-o test_project/build/local/module_" + std::to_string(i) + ".o";
        if (!run_command(command)) {
            throw std::runtime_error("local compile failed for module_" + std::to_string(i));
        }
    }

    if (!run_command(
            "clang++ -std=c++23 -O2 -fconstexpr-steps=4000000 "
            "-Itest_project/include "
            "-c test_project/src/main.cpp "
            "-o test_project/build/local/main.o")) {
        throw std::runtime_error("local compile failed for main.cpp");
    }

    if (!run_command(
            "clang++ -std=c++23 -O2 "
            "test_project/build/local/module_*.o "
            "test_project/build/local/main.o "
            "-o test_project/build/local/test_project_app")) {
        throw std::runtime_error("local link failed");
    }

    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

double run_distributed_build(orion::ClusterHead::Stub& stub) {
    clean_distributed_outputs();

    const auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < kNumModules; ++i) {
        orion::TaskRequest req;
        req.set_task_id("test_project_module_" + std::to_string(i));
        req.set_function_name("compile_test_project_unit");
        req.add_args(pack_int(i));

        orion::TaskReply reply;
        grpc::ClientContext ctx;
        auto status = stub.SubmitTask(&ctx, req, &reply);
        if (!status.ok()) {
            throw std::runtime_error("SubmitTask failed for module_" + std::to_string(i) + ": " + status.error_message());
        }
    }

    {
        orion::TaskRequest req;
        req.set_task_id("test_project_main");
        req.set_function_name("compile_test_project_main");
        for (int i = 0; i < kNumModules; ++i) {
            req.add_dep_ids("test_project_module_" + std::to_string(i));
        }

        orion::TaskReply reply;
        grpc::ClientContext ctx;
        auto status = stub.SubmitTask(&ctx, req, &reply);
        if (!status.ok()) {
            throw std::runtime_error("SubmitTask failed for main.cpp: " + status.error_message());
        }
    }

    {
        orion::TaskRequest req;
        req.set_task_id("test_project_link");
        req.set_function_name("link_test_project_exec");
        req.add_dep_ids("test_project_main");
        for (int i = 0; i < kNumModules; ++i) {
            req.add_dep_ids("test_project_module_" + std::to_string(i));
        }

        orion::TaskReply reply;
        grpc::ClientContext ctx;
        auto status = stub.SubmitTask(&ctx, req, &reply);
        if (!status.ok()) {
            throw std::runtime_error("SubmitTask failed for link step: " + status.error_message());
        }
    }

    const fs::path output = "test_project/build/distributed/test_project_app";
    while (!fs::exists(output)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::string head_port = (argc > 1) ? argv[1] : "50050";
    const std::string label = (argc > 2) ? argv[2] : "cluster";
    const std::string target = "localhost:" + head_port;

    generate_test_project();

    auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
    auto stub = orion::ClusterHead::NewStub(channel);

    std::cout << "=== test_project benchmark (" << label << ") ===\n";
    std::cout << "Project: test_project/\n";
    std::cout << "Modules: " << kNumModules << "\n";

    const double local_ms = run_local_build();
    std::cout << "[Local sequential] " << local_ms << " ms\n";

    const double distributed_ms = run_distributed_build(*stub);
    std::cout << "[Orion " << label << "] " << distributed_ms << " ms\n";

    const double speedup = local_ms / distributed_ms;
    const double reduction = ((local_ms - distributed_ms) / local_ms) * 100.0;

    std::cout << "Speedup vs local: " << speedup << "x\n";
    std::cout << "Time reduction vs local: " << reduction << "%\n";

    return 0;
}
