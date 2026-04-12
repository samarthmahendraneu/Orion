//
// Created by Samarth Mahendra on 12/21/25.
//

#include "object_store.h"
#include "../distributed/cluster/global_object_store.h"
#include <iostream>


namespace orion {

    void ObjectStore::put(const ObjectId& id, std::any value, const std::string& node_id) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            store_[id] = value; // Copy into local store, don't move so it can be broadcast
        }

        // Notify waiting threads
        cv_.notify_all();

        // Trigger callback if registered (notify local scheduler)
        if (on_put_callback_) {
            on_put_callback_(id);
        }

        // Phase 3 Global Store: Broadcast to central scheduler
        if (global_store_) {
            global_store_->put_object(id, value);
        }
    }

    void ObjectStore::set_global_context(orion::distributed::GlobalObjectStore* global_store) {
        std::lock_guard<std::mutex> lock(mutex_);
        global_store_ = global_store;
    }

    void ObjectStore::set_on_put_callback(OnPutCallback callback) {
        on_put_callback_ = std::move(callback);
    }

    std::optional<std::any> ObjectStore::get(const ObjectId& id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = store_.find(id);
        if (it != store_.end()) {
            return it->second;
        }
        
        // Check global store if not found locally
        if (global_store_) {
            auto val_opt = global_store_->get_object(id);
            if (val_opt.has_value()) {
                store_[id] = val_opt.value(); // cache it locally
                return store_[id];
            }
        }
        return std::nullopt;
    }

    std::any ObjectStore::get_blocking(const ObjectId& id) {
        std::unique_lock<std::mutex> lock(mutex_);

        // 1. Wait until the object appears locally or globally
        cv_.wait(lock, [&] {
            if (store_.find(id) != store_.end()) {
                return true;
            }
            if (global_store_ && global_store_->get_object(id).has_value()) {
                return true;
            }
            return false;
        });

        // 2. Return local object if it appeared locally
        if (store_.find(id) != store_.end()) {
            return store_[id];
        }

        // 3. Otherwise we woke up because it's in the global store. Cache it locally and return.
        if (global_store_) {
            auto val_opt = global_store_->get_object(id);
            if (val_opt.has_value()) {
                store_[id] = val_opt.value();
                return store_[id];
            }
        }

        throw std::runtime_error("Object missing from both local and global store");
    }

}