//
// Created by Samarth Mahendra on 12/21/25.
//

#include "object_store.h"


namespace orion {

    void ObjectStore::put(const ObjectId& id, std::any value) {
        std::lock_guard<std::mutex> lock(mutex_);
        store_[id] = std::move(value);
    }

    std::optional<std::any> ObjectStore::get(const ObjectId& id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = store_.find(id);
        if (it != store_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

}