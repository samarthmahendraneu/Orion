#pragma once

#include <string>
#include <any>

namespace orion::distributed {

    class GlobalObjectStore {
    public:
        virtual ~GlobalObjectStore() = default;

        // Where does this object live?
        virtual std::optional<std::any> get_object(const std::string& object_id) = 0;

        // Called by NodeRuntime when a worker completes a task
        virtual void put_object(const std::string& object_id, std::any value) = 0;
    };

} // namespace orion::distributed
