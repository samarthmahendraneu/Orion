#pragma once

#include <string>
#include <fstream>
#include <filesystem>
#include "../functions/hash_util.h"

namespace orion::distributed {

namespace fs = std::filesystem;

class CasStore {
public:
    explicit CasStore(const std::string& root_dir = "./storage/cas")
        : root_(root_dir) {
        fs::create_directories(root_ / "blob");
        fs::create_directories(root_ / "actions");
    }

    // Stores a data blob in the CAS, returns its content hash.
    std::string put_blob(const std::string& data) {
        std::string hash = compute_sha256(data);
        fs::path p = blob_path(hash);
        if (!fs::exists(p)) {
            std::ofstream out(p, std::ios::binary);
            out.write(data.data(), data.size());
        }
        return hash;
    }

    // Associates an action hash with an output object hash.
    void link_action(const std::string& action_hash, const std::string& output_hash) {
        std::ofstream out(root_ / "actions" / action_hash);
        out << output_hash;
    }

    // Looks up the output hash for a given action hash.
    std::string lookup_action(const std::string& action_hash) {
        std::ifstream in(root_ / "actions" / action_hash);
        std::string out_hash;
        if (in >> out_hash) return out_hash;
        return "";
    }

    // Checks if a blob exists.
    bool has_blob(const std::string& hash) {
        return fs::exists(blob_path(hash));
    }

    // Reads a blob's content.
    std::string get_blob(const std::string& hash) {
        std::ifstream in(blob_path(hash), std::ios::binary);
        if (!in) return "";
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

private:
    fs::path blob_path(const std::string& hash) {
        // Use a 2-level directory structure to avoid thousands of files in one dir.
        std::string prefix = hash.substr(0, 2);
        fs::path p = root_ / "blob" / prefix;
        fs::create_directories(p);
        return p / hash;
    }

private:
    fs::path root_;
};

} // namespace orion::distributed
