#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <any>

#include "object_ref.h"

namespace orion {

    struct Task {
        std::string id;
        std::string function_name;       // wire-safe name; looked up in FunctionRegistry on remote nodes
        std::vector<std::string> args;   // serialized literal args (4-byte LE ints for now); forwarded to nodes
        std::vector<ObjectRef> deps;
        
        // V2: Content-addressed dependencies (Filename -> Hash)
        std::map<std::string, std::string> input_map;

        Task() = default;   // 👈 allows `Task task;`

        // ALWAYS takes dependency values
        std::function<std::any(std::vector<std::any>)> work;

        // Task with deps (closure version — local use)
        Task(std::string id,
             std::vector<ObjectRef> deps,
             std::function<std::any(std::vector<std::any>)> fn)
            : id(std::move(id)),
              deps(std::move(deps)),
              work(std::move(fn)) {}

        // Task without deps (closure version — local use)
        Task(std::string id,
             std::vector<ObjectRef> deps,
             std::function<std::any()> fn)
            : id(std::move(id)),
              deps(std::move(deps)),
              work([fn = std::move(fn)](std::vector<std::any>) {
                    return fn();
                }) {}
    };

} // namespace orion
