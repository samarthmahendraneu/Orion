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
    std::shared_future<bool> future;
};

// Compute Merkle hashes recursively
std::string compute_action_hash(const std::map<std::string, std::unique_ptr<DAGNode>>& nodes,
                                const std::string& target,
                                std::map<std::string, std::string>& cache) {
    if (cache.count(target)) return cache[target];

    auto it = nodes.find(target);
    if (it == nodes.end() || it->second->command.empty()) {
        // It's a leaf file, hash its contents if it exists locally
        if (fs::exists(target)) {
            std::ifstream in(target, std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            std::string h = "FILE:" + orion::distributed::compute_sha256(content);
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

// Perform gRPC submission with Raft redirection
bool grpc_submit(DAGNode* node, std::string current_target, 
                 const std::string& expected_hash,
                 const std::map<std::string, std::string>& hashes,
                 std::shared_ptr<orion::distributed::CasClient> cas_client) {
    auto channel = grpc::CreateChannel(current_target, grpc::InsecureChannelCredentials());
    auto stub = orion::ClusterHead::NewStub(channel);

    int retries = 0;
    while (retries < 15) {
        orion::TaskRequest req;
        req.set_task_id(node->id);
        req.set_function_name("shell_execute");
        
        std::string cmd = node->command;
        req.add_args(cmd);
        req.set_expected_hash(expected_hash);
        req.set_working_dir(node->working_dir);

        // V2: Populate input map from dependencies
        for (auto* dep : node->deps) {
            if (dep->command.empty()) {
                // Leaf file (source) - must be uploaded to CAS
                if (hashes.count(dep->id)) {
                    (*req.mutable_input_map())[dep->id] = hashes.at(dep->id);
                }
            } else {
                // Computed dependency - head already knows its canonical hash from previous steps
                // We just list the ID, head's scheduler will resolve the hash during dispatch.
                req.add_dep_ids(dep->id);
            }
        }

        orion::TaskReply reply;
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(20));
        
        grpc::Status status = stub->SubmitTask(&ctx, req, &reply);
        if (status.ok()) {
            if (reply.accepted()) {
                if (!reply.output_hash().empty()) {
                    std::cout << "[Orion] Task " << node->id << ": CACHE HIT (hash=" << reply.output_hash().substr(0, 16) << ")\n";
                    return true;
                } else {
                    std::cout << "[Orion] Task " << node->id << ": EXECUTING...\n";
                    return false;
                }
            }
            if (!reply.leader_address().empty() && reply.leader_address() != current_target) {
                current_target = reply.leader_address();
                channel = grpc::CreateChannel(current_target, grpc::InsecureChannelCredentials());
                stub = orion::ClusterHead::NewStub(channel);
                // Also update cas_client's internally? 
                // For this POC we'll assume head has a unified load balanced service or just retry.
                continue;
            }
        }
        
        retries++;
        std::this_thread::sleep_for(std::chrono::milliseconds(200 + (std::rand() % 100)));
    }
    throw std::runtime_error("SubmitTask rejected after 15 retries for node " + node->id);
}

std::shared_future<bool> submit_node(DAGNode* node, const std::string& head_addr, 
                                     const std::map<std::string, std::string>& hashes,
                                     std::shared_ptr<orion::distributed::CasClient> cas_client) {
    std::lock_guard<std::mutex> lock(node->mu);
    if (node->started) return node->future;
    
    node->started = true;
    node->future = std::async(std::launch::async, [=, &hashes]() -> bool {
        std::vector<std::shared_future<bool>> dep_futs;
        for (auto* dep : node->deps) {
            dep_futs.push_back(submit_node(dep, head_addr, hashes, cas_client));
        }
        for (auto& f : dep_futs) f.get(); // wait for dependencies

        if (node->command.empty()) return true; // Leaf file, no task to run
        return grpc_submit(node, head_addr, hashes.at(node->id), hashes, cas_client);
    });
    return node->future;
}

int main(int argc, char* argv[]) {
    std::string head_addr = "127.0.0.1:50050";
    std::string dir = ".";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--head" && i + 1 < argc) head_addr = argv[++i];
        else if (arg == "--dir" && i + 1 < argc) dir = argv[++i];
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
    std::string first_target = "";

    std::ifstream file(makefile_path);
    std::string line;
    DAGNode* current_node = nullptr;

    std::regex target_regex("^([a-zA-Z0-9_\\-\\./]+)\\s*:\\s*(.*)$");

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
                current_node->dep_names.push_back(dep);
            }
        } else if (line[0] == '\t' && current_node) {
            current_node->command = line.substr(1); // strip tab
        }
    }

    if (first_target.empty()) {
        std::cerr << "Error: No targets found in Makefile\n";
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

    // Hash Action Merkle Tree
    std::map<std::string, std::string> action_hashes;
    compute_action_hash(nodes, first_target, action_hashes);

    auto start_time = std::chrono::steady_clock::now();

    // V2: Initialize CAS Client
    auto cas_channel = grpc::CreateChannel(head_addr, grpc::InsecureChannelCredentials());
    auto cas_client = std::make_shared<orion::distributed::CasClient>(cas_channel);

    // V2: Pre-upload all leaf nodes sequentially to avoid gRPC connection exhaustion
    std::cout << "[Orion CLI] Uploading source files to CAS...\n";
    for (auto& [id, node] : nodes) {
        if (node->command.empty() && fs::exists(id)) {
            std::string hash = cas_client->upload_blob(id);
            if (!hash.empty()) {
                action_hashes[id] = hash; // Stuff leaf hashes here for convenience
            } else {
                std::cerr << "[Orion CLI] WARNING: Failed to upload source file " << id << "\n";
            }
        }
    }

    // Submit recursively
    bool final_cached = submit_node(nodes[first_target].get(), head_addr, action_hashes, cas_client).get();

    if (!final_cached && !first_target.empty()) {
        double wait_seconds = 0.0;
        while (!fs::exists(fs::path(dir) / first_target) && wait_seconds < 600.0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            wait_seconds += 0.2;
        }
        if (!fs::exists(fs::path(dir) / first_target)) {
            std::cerr << "[Orion CLI] ERROR: Timed out waiting for final artifact "
                      << (fs::path(dir) / first_target) << "\n";
            return 1;
        }
    }

    auto end_time = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

    std::cout << "----------------------------------------\n";
    if (final_cached) {
         std::cout << "[Orion CLI] Build completed perfectly. (CACHE HIT)\n";
    } else {
         std::cout << "[Orion CLI] Build completed. (EXECUTED)\n";
    }
    std::cout << "[Orion CLI] Total Time: " << ms << " ms\n";

    return 0;
}
