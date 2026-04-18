#pragma once

#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include "../functions/hash_util.h"
#include "../../core/task.h"

namespace orion::distributed {

class ActionCache {
public:
    // Computes a deterministic SHA-256 hash for a task based on its identity.
    // In a Merkle-tree approach, the action hash of a task depends on the 
    // ACTION HASHES of its dependencies (instead of transient result hashes). 
    // This allows the client to compute all keys upfront and avoids race conditions.
    static std::string compute_action_hash(
        const std::string& function_name,
        const std::vector<std::string>& args,
        const std::map<std::string, std::string>& dep_action_hashes,
        const std::string& platform = "darwin-arm64") 
    {
        std::stringstream ss;
        ss << "orion/v7" << "|"; // Versioning the hash format
        ss << function_name << "|";
        
        // Args
        for (const auto& arg : args) {
            ss << arg << "|";
        }

        // Dependency Action Hashes (sorted for determinism)
        std::vector<std::string> sorted_deps;
        for (const auto& [id, action_hash] : dep_action_hashes) {
            sorted_deps.push_back(id + "::" + action_hash);
        }
        std::sort(sorted_deps.begin(), sorted_deps.end());
        for (const auto& dep : sorted_deps) {
            ss << dep << "|";
        }

        ss << platform;

        return compute_sha256(ss.str());
    }
};

} // namespace orion::distributed
