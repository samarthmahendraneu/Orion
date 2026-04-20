//
// Created by Samarth Mahendra on 2/28/26.
//

#ifndef BUILTIN_FUNCTIONS_H
#define BUILTIN_FUNCTIONS_H
#pragma once
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include "function_registry.h"

namespace orion::distributed {

    inline void register_builtin_functions(FunctionRegistry& registry) {
        registry.register_function("add",
            [](std::vector<std::any> args) -> std::any {
                if (args.size() < 2) return 0;
                try {
                    int a = std::any_cast<int>(args[0]);
                    int b = std::any_cast<int>(args[1]);
                    return a + b;
                } catch (...) { return 0; }
            });

        registry.register_function("identity",
            [](std::vector<std::any> args) -> std::any {
                if (args.empty()) return std::any();
                return args[0];
            });

        // shell_execute is now a first-class registered function.
        // NodeServiceImpl will invoke this.
        registry.register_function("shell_execute",
            [](std::vector<std::any> args) -> std::any {
                if (args.size() < 2) return 0;
                std::string cmd = std::any_cast<std::string>(args[0]);
                std::string sandbox = std::any_cast<std::string>(args[1]);
                
                std::string full_cmd = "cd " + sandbox + " && " + cmd;
                int ret = std::system(full_cmd.c_str());
                return (ret == 0) ? 1 : 0;
            });
    }
}
#endif //BUILTIN_FUNCTIONS_H
