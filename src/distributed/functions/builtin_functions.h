//
// Created by Samarth Mahendra on 2/28/26.
//

#ifndef BUILTIN_FUNCTIONS_H
#define BUILTIN_FUNCTIONS_H
#pragma once
#include <stdexcept>
#include "function_registry.h"

namespace orion::distributed {

    inline void register_builtin_functions(FunctionRegistry& registry) {

        registry.register_function("add",
            [](std::vector<std::any> args) -> std::any {
                if (args.size() < 2)
                    throw std::runtime_error("add: expected 2 args, got " +
                                             std::to_string(args.size()));
                int a = std::any_cast<int>(args[0]);
                int b = std::any_cast<int>(args[1]);
                return a + b;
            });

        registry.register_function("mul",
            [](std::vector<std::any> args) -> std::any {
                if (args.size() < 2)
                    throw std::runtime_error("mul: expected 2 args, got " +
                                             std::to_string(args.size()));
                int a = std::any_cast<int>(args[0]);
                int b = std::any_cast<int>(args[1]);
                return a * b;
            });

        registry.register_function("compile",
            [](std::vector<std::any> args) -> std::any {
                if (args.empty()) return 0;
                int i = std::any_cast<int>(args[0]);
                std::string cmd = "clang++ -std=c++23 -O2 -c bench_src_" + std::to_string(i) + ".cpp -o dist_obj_" + std::to_string(i) + ".o";
                int ret = std::system(cmd.c_str());
                return (ret == 0) ? 1 : 0;
            });

    }

}
#endif //BUILTIN_FUNCTIONS_H
