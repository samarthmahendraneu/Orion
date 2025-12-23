//
// Created by Samarth Mahendra on 12/21/25.
//

#ifndef OBJECT_STORE_H
#define OBJECT_STORE_H
#include <mutex>
#include <unordered_map>
#include <any>
#include <optional>
#include <functional>

#include "object_ref.h"


namespace orion {

    // Simple in-memory object store.
    // Maps ObjectId -> std::any
    class ObjectStore {
    public:
        using OnPutCallback = std::function<void(const ObjectId&)>;

        void put(const ObjectId& id, std::any value);
        std::optional<std::any> get(const ObjectId& id);
        // Blocking get: waits until object exists
        std::any get_blocking(const ObjectId& id);

        // Register callback to be invoked when objects are created
        void set_on_put_callback(OnPutCallback callback);

    private:
        std::unordered_map<ObjectId, std::any> store_;
        std::mutex mutex_;
        std::condition_variable cv_;
        OnPutCallback on_put_callback_;
    };


}



#endif //OBJECT_STORE_H
