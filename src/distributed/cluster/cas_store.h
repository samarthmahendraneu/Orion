#pragma once

#include <string>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <algorithm>
#include <vector>
#include <set>
#include <iostream>
#include <mutex>
#include "../functions/hash_util.h"

namespace orion::distributed {

namespace fs = std::filesystem;

class CasStore {
public:
    explicit CasStore(const std::string& root_dir = "./storage/cas",
                     std::chrono::seconds ttl = std::chrono::hours(24),
                     size_t max_entries = 5000)
        : root_(root_dir), ttl_(ttl), max_entries_(max_entries) {
        std::lock_guard<std::mutex> lock(mtx_);
        fs::create_directories(root_ / "blob");
        fs::create_directories(root_ / "actions");
    }

    // Stores a data blob in the CAS, returns its content hash.
    std::string put_blob(const std::string& data) {
        std::string hash = compute_sha256(data);
        std::lock_guard<std::mutex> lock(mtx_);
        fs::path p = blob_path(hash, false); // false = don't lock recursively
        if (!fs::exists(p)) {
            std::ofstream out(p, std::ios::binary);
            out.write(data.data(), data.size());
        }
        return hash;
    }

    // Associates an action hash with an output object hash.
    void link_action(const std::string& action_hash, const std::string& output_hash) {
        std::lock_guard<std::mutex> lock(mtx_);
        std::ofstream out(root_ / "actions" / action_hash);
        out << output_hash;
    }

    // Looks up the output hash for a given action hash.
    // Refreshes last_write_time to track "Last Access" for LRU.
    std::string lookup_action(const std::string& action_hash) {
        std::lock_guard<std::mutex> lock(mtx_);
        fs::path p = root_ / "actions" / action_hash;
        if (!fs::exists(p)) return "";

        // Refresh access time for LRU
        std::error_code ec;
        fs::last_write_time(p, fs::file_time_type::clock::now(), ec);

        std::ifstream in(p);
        std::string out_hash;
        if (in >> out_hash) return out_hash;
        return "";
    }

    // Performs a maintenance sweep:
    // 1. Evicts actions older than TTL.
    // 2. Evicts oldest actions if count exceeds max_entries.
    // 3. Removes blobs that are no longer referenced by any actions.
    void sweep() {
        std::lock_guard<std::mutex> lock(mtx_);
        std::vector<std::pair<fs::file_time_type, fs::path>> entries;
        std::set<std::string> referenced_blobs;
        auto now = fs::file_time_type::clock::now();

        // Pass 1: Collect actions and apply TTL
        for (const auto& entry : fs::directory_iterator(root_ / "actions")) {
            if (!entry.is_regular_file()) continue;

            auto lwt = entry.last_write_time();
            auto age = std::chrono::duration_cast<std::chrono::seconds>(now - lwt);

            if (age >= ttl_) {
                fs::remove(entry.path());
                continue;
            }
            entries.push_back({lwt, entry.path()});
        }

        // Pass 2: LRU Eviction (by entry count)
        if (entries.size() > max_entries_) {
            std::sort(entries.begin(), entries.end()); // Oldest first
            size_t to_remove = entries.size() - max_entries_;
            for (size_t i = 0; i < to_remove; ++i) {
                fs::remove(entries[i].second);
            }
            // Update the entries list to reflect current state for Blob GC
            entries.erase(entries.begin(), entries.begin() + to_remove);
        }

        // Pass 3: Blob Garbage Collection
        // First, re-read all current actions to find referenced blobs
        for (const auto& entry : fs::directory_iterator(root_ / "actions")) {
            std::ifstream in(entry.path());
            std::string blob_hash;
            if (in >> blob_hash) {
                referenced_blobs.insert(blob_hash);
            }
        }

        // Sweep blobs (2-level directory)
        for (const auto& dir : fs::directory_iterator(root_ / "blob")) {
            if (!dir.is_directory()) continue;
            for (const auto& blob : fs::directory_iterator(dir.path())) {
                if (!blob.is_regular_file()) continue;
                
                std::string hash = blob.path().filename().string();
                if (referenced_blobs.find(hash) == referenced_blobs.end()) {
                    // Blob is unreferenced. Delete if it's at least 1 hour old
                    // to avoid races with concurrent writers.
                    auto lwt = blob.last_write_time();
                    if (std::chrono::duration_cast<std::chrono::hours>(now - lwt) >= std::chrono::hours(1)) {
                        fs::remove(blob.path());
                    }
                }
            }
        }
    }

    // Checks if a blob exists.
    bool has_blob(const std::string& hash) {
        std::lock_guard<std::mutex> lock(mtx_);
        return fs::exists(blob_path(hash, false));
    }

    // Reads a blob's content.
    std::string get_blob(const std::string& hash) {
        std::lock_guard<std::mutex> lock(mtx_);
        std::ifstream in(blob_path(hash, false), std::ios::binary);
        if (!in) return "";
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

private:
    fs::path blob_path(const std::string& hash, bool lock = true) {
        // Option to skip locking if the caller already holds the mutex
        std::unique_ptr<std::lock_guard<std::mutex>> lk;
        if (lock) lk = std::make_unique<std::lock_guard<std::mutex>>(mtx_);

        // Use a 2-level directory structure to avoid thousands of files in one dir.
        std::string prefix = hash.substr(0, 2);
        fs::path p = root_ / "blob" / prefix;
        fs::create_directories(p);
        return p / hash;
    }

private:
    fs::path root_;
    std::chrono::seconds ttl_;
    size_t max_entries_;
    std::mutex mtx_;
};

} // namespace orion::distributed
