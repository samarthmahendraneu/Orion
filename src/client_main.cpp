#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <future>
#include <mutex>
#include <fstream>
#include <sstream>
#include <regex>
#include <filesystem>
#include <thread>
#include <chrono>
#include <set>

#include <grpcpp/grpcpp.h>
#include "distributed/generated/orion.grpc.pb.h"
#include "distributed/functions/hash_util.h"
#include "distributed/rpc/cas_client.h"

namespace fs = std::filesystem;

struct DAGNode {
    std::string id;
    std::string command;
    std::string working_dir;   // absolute path sent to nodes
    std::vector<std::string> dep_names;
    std::vector<DAGNode*> deps;

    std::mutex mu;
    bool started = false;
    std::shared_future<std::string> future;
};

// Generic file hashing logic simplified.
// No extra header scanning needed as we upload the entire folder context.

// Compute Merkle hashes recursively
std::string compute_action_hash(const std::map<std::string, std::unique_ptr<DAGNode>>& nodes,
                                const std::string& target,
                                std::map<std::string, std::string>& cache) {
    if (cache.count(target)) return cache[target];

    auto it = nodes.find(target);
    bool is_leaf = (it == nodes.end()) || (it->second->command.empty() && it->second->dep_names.empty());

    if (is_leaf) {
        // It's a leaf file, hash its contents if it exists locally
        if (fs::exists(target)) {
            std::ifstream in(target, std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            
            // Simplified content-based hashing for all files.
            // The "Global Context" strategy will ensure all necessary headers
            // are uploaded to the CAS and available in the sandbox.
            std::string header_hash_input = content;
            // No more recursive header scanning - we rely on global context uplift.

            std::string h = "FILE:" + orion::distributed::compute_sha256(header_hash_input);
            cache[target] = h;
            return h;
        } else {
            // Virtual node or missing file
            std::string h = "VIRTUAL:" + target;
            cache[target] = h;
            return h;
        }
    }

    std::string input = it->second->command;
    for (const auto& dep : it->second->dep_names) {
        input += compute_action_hash(nodes, dep, cache);
    }
    
    std::string h = orion::distributed::compute_sha256(input);
    cache[target] = h;
    return h;
}

// Perform gRPC submission and poll for completion
std::string grpc_submit(DAGNode* node,
                        orion::ClusterHead::Stub* stub,
                        const std::string& expected_hash,
                        const std::map<std::string, std::string>& hashes,
                        std::shared_ptr<orion::distributed::CasClient> cas_client,
                        int max_retries,
                        int timeout_sec) {
    int retries = 0;
    while (retries < max_retries) {
        orion::TaskRequest req;
        req.set_task_id(node->id);
        req.set_function_name("shell_execute");
        
        std::string cmd = node->command;
        req.add_args(cmd);
        req.set_expected_hash(expected_hash);
        req.set_working_dir(node->working_dir);

        // V2: Populate input map with ALL source/header files (Global Context strategy)
        // This ensures headers like lua.h are always found regardless of include flags.
        for (auto const& [file_path, file_hash] : hashes) {
            std::string ext = fs::path(file_path).extension().string();
            if (ext == ".c" || ext == ".h" || ext == ".cpp" || ext == ".hpp") {
                (*req.mutable_input_map())[file_path] = file_hash;
            }
        }
        
        // Also ensure direct computed dependencies are listed
        for (auto* dep : node->deps) {
            if (!dep->command.empty() || !dep->dep_names.empty()) {
                req.add_dep_ids(dep->id);
            }
        }

        orion::TaskReply reply;
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(timeout_sec));
        
        grpc::Status status = stub->SubmitTask(&ctx, req, &reply);
        if (status.ok()) {
            if (reply.accepted()) {
                if (!reply.output_hash().empty()) {
                    return reply.output_hash(); // Cache hit
                }
                
                // Poll for completion
                while (true) {
                    orion::TaskStatusRequest stat_req;
                    stat_req.set_task_id(node->id);
                    orion::TaskStatusReply stat_reply;
                    grpc::ClientContext stat_ctx;
                    stat_ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
                    
                    grpc::Status s = stub->GetTaskStatus(&stat_ctx, stat_req, &stat_reply);
                    if (s.ok() && stat_reply.finished()) {
                        if (stat_reply.failed()) {
                            throw std::runtime_error("Task " + node->id + " failed: " + stat_reply.error_message());
                        }
                        return stat_reply.output_hash();
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        }
        
        retries++;
    }
    throw std::runtime_error("SubmitTask rejected or timed out for node " + node->id);
}

// Submit a single node recursively
std::shared_future<std::string> submit_node(DAGNode* node, 
                                            orion::ClusterHead::Stub* stub,
                                            const std::map<std::string, std::string>& hashes,
                                            std::shared_ptr<orion::distributed::CasClient> cas_client,
                                            int retries,
                                            int timeout) {
    std::lock_guard<std::mutex> lock(node->mu);
    if (node->started) return node->future;
    node->started = true;
    
    node->future = std::async(std::launch::async, [=, &hashes]() -> std::string {
        std::vector<std::shared_future<std::string>> dep_futs;
        for (auto* dep : node->deps) {
            dep_futs.push_back(submit_node(dep, stub, hashes, cas_client, retries, timeout));
        }
        
        std::string last_dep_hash = "";
        for (auto& f : dep_futs) {
            last_dep_hash = f.get(); // wait for dependencies
        }

        if (node->command.empty()) {
            if (node->dep_names.empty()) {
                // True leaf file
                auto h_it = hashes.find(node->id);
                if (h_it == hashes.end()) throw std::runtime_error("No hash for leaf " + node->id);
                return h_it->second;
            } else {
                // Virtual/Phoney target - just return the action hash or last dep hash
                auto h_it = hashes.find(node->id);
                return (h_it != hashes.end()) ? h_it->second : last_dep_hash;
            }
        }
        
        auto h_it = hashes.find(node->id);
        if (h_it == hashes.end()) throw std::runtime_error("No hash for task " + node->id);
        return grpc_submit(node, stub, h_it->second, hashes, cas_client, retries, timeout);
    });
    return node->future;
}

int main(int argc, char* argv[]) {
    std::string head_addr = "127.0.0.1:50050";
    std::string dir = ".";
    std::string custom_target = "";
    int max_retries = 15;
    int timeout_sec = 20;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--head" && i + 1 < argc) head_addr = argv[++i];
        else if (arg == "--dir" && i + 1 < argc) dir = argv[++i];
        else if (arg == "--retries" && i + 1 < argc) max_retries = std::stoi(argv[++i]);
        else if (arg == "--timeout" && i + 1 < argc) timeout_sec = std::stoi(argv[++i]);
        else if (arg[0] != '-') custom_target = arg;
    }

    fs::path abs_dir = fs::absolute(dir);
    fs::path makefile_path = abs_dir / "Makefile";
    if (!fs::exists(makefile_path)) {
        std::cerr << "Error: No Makefile found in " << dir << "\n";
        return 1;
    }
    std::string working_dir = abs_dir.string();

    std::cout << "[Orion CLI] Parsing " << makefile_path << "...\n";

    std::map<std::string, std::unique_ptr<DAGNode>> nodes;
    std::map<std::string, std::string> vars;
    std::string first_target = "";

    std::ifstream file(makefile_path);
    std::string line;
    DAGNode* current_node = nullptr;

    std::regex target_regex("^([a-zA-Z0-9_\\-\\./]+)\\s*:\\s*(.*)$");
    std::regex var_regex("^([a-zA-Z0-9_]+)\\s*=\\s*(.*)$");

    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        std::smatch match;
        if (std::regex_match(line, match, target_regex)) {
            std::string target = match[1];
            if (first_target.empty()) first_target = target;
            
            if (!nodes.count(target)) nodes[target] = std::make_unique<DAGNode>();
            current_node = nodes[target].get();
            current_node->id = target;
            current_node->working_dir = working_dir;
            
            std::string deps_str = match[2];
            std::stringstream ss(deps_str);
            std::string dep;
            while (ss >> dep) {
                // Expand variables in dependencies if any (uncommon but possible)
                current_node->dep_names.push_back(dep);
            }
        } else if (std::regex_match(line, match, var_regex)) {
            vars[match[1]] = match[2];
            current_node = nullptr;
        } else if (line[0] == '\t' && current_node) {
            std::string cmd = line.substr(1);
            // Simple variable expansion $(VAR)
            for (auto const& [name, val] : vars) {
                std::string placeholder = "$(" + name + ")";
                size_t pos = 0;
                while ((pos = cmd.find(placeholder, pos)) != std::string::npos) {
                    cmd.replace(pos, placeholder.length(), val);
                    pos += val.length();
                }
            }
            current_node->command = cmd;
        }
    }

    std::string target = custom_target.empty() ? first_target : custom_target;
    if (!nodes.count(target)) {
        std::cerr << "Error: Target '" << target << "' not found in Makefile\n";
        return 1;
    }

    // Link graph
    for (auto& [id, node] : nodes) {
        for (const auto& dep_name : node->dep_names) {
            if (!nodes.count(dep_name)) {
                auto new_node = std::make_unique<DAGNode>();
                new_node->id = dep_name;
                nodes[dep_name] = std::move(new_node);
            }
            node->deps.push_back(nodes[dep_name].get());
        }
    }

    std::cout << "[Orion CLI] Target: " << first_target << " | Nodes: " << nodes.size() << "\n";
    for (auto const& [name, node] : nodes) {
        if (!node->command.empty()) {
            std::cout << "  Task: " << name << " [" << node->command.substr(0, 32) << "...]\n";
            for (auto* d : node->deps) std::cout << "    -> " << d->id << "\n";
        }
    }
    std::map<std::string, std::string> action_hashes;
    compute_action_hash(nodes, first_target, action_hashes);

    auto start_time = std::chrono::steady_clock::now();

    // V2: Initialize CAS Client and Cluster Stub
    auto head_channel = grpc::CreateChannel(head_addr, grpc::InsecureChannelCredentials());
    auto cas_client = std::make_shared<orion::distributed::CasClient>(head_channel);
    auto head_stub = orion::ClusterHead::NewStub(head_channel);

    // V2: Pre-scan and Uplift all files in the project folder
    std::cout << "[Orion CLI] Uplifting all project files to CAS (Global Context)...\n";
    for (const auto& entry : fs::recursive_directory_iterator(abs_dir)) {
        if (entry.is_regular_file()) {
            std::string ext = entry.path().extension().string();
            // Uplift all source/header files to ensure complete build context
            if (ext == ".c" || ext == ".h" || ext == ".cpp" || ext == ".hpp" || ext == ".o" || ext == ".a") {
                std::string h = cas_client->upload_blob(entry.path().string());
                if (!h.empty()) {
                    fs::path rel = fs::relative(entry.path(), abs_dir);
                    action_hashes[rel.string()] = h;
                }
            }
        }
    }

    // Submit recursively
    std::string final_hash = "";
    try {
        final_hash = submit_node(nodes[target].get(), head_stub.get(), action_hashes, cas_client, max_retries, timeout_sec).get();
    } catch (const std::exception& e) {
        std::cerr << "[Orion CLI] Build failed with exception: " << e.what() << "\n";
        return 1;
    }

    if (!final_hash.empty()) {
        fs::path dest = fs::path(dir) / target;
        if (!fs::exists(dest)) {
            std::cout << "[Orion CLI] Downloading final artifact: " << target << " (" << final_hash.substr(0,16) << ")\n";
            if (!cas_client->fetch_blob(final_hash, dest)) {
                std::cerr << "[Orion CLI] ERROR: Failed to download final artifact.\n";
                return 1;
            }
        }
    }

    auto end_time = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

    std::cout << "----------------------------------------\n";
    if (!final_hash.empty()) {
         std::cout << "[Orion CLI] Build completed successfully.\n";
    } else {
         std::cout << "[Orion CLI] Build failed.\n";
    }
    std::cout << "[Orion CLI] Total Time: " << ms << " ms\n";

    return 0;
}
