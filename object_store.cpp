//
// Created by Samarth Mahendra on 12/21/25.
//

#include "object_store.h"


namespace orion {

    void ObjectStore::put(const ObjectId& id, std::any value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            store_[id] = std::move(value);
        }

        // Notify waiting threads
        cv_.notify_all();

        // Trigger callback if registered (notify scheduler)
        if (on_put_callback_) {
            on_put_callback_(id);
        }
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
        return std::nullopt;
    }

    std::any ObjectStore::get_blocking(const ObjectId& id) {
        std::unique_lock<std::mutex> lock(mutex_);

        // Wait until the object appears
        cv_.wait(lock, [&] {
            return store_.find(id) != store_.end();
        });

        return store_[id];
    }

}