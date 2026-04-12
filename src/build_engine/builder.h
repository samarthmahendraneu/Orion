#pragma once

#include "build_target.h"
#include "../core/object_ref.h"
#include "../core/task.h"
#include "../core/context.h"
#include <cstdlib>
#include <iostream>

namespace orion::build_engine {

template <typename SchedulerType>
class Builder {
public:
    Builder(SchedulerType& scheduler) : sched_(scheduler) {}

    orion::ObjectRef build(const BuildTarget& target) {
        std::vector<orion::ObjectRef> object_refs;
        std::vector<std::string> object_files;

        for (const auto& src : target.sources) {
            size_t dot_pos = src.find_last_of('.');
            std::string base_name = (dot_pos == std::string::npos) ? src : src.substr(0, dot_pos);
            std::string obj_file = base_name + ".o";

            object_files.push_back(obj_file);

            orion::Task compile_task{
                "compile_" + src,
                {},
                [src, obj_file, cxx=target.cxx, flags=target.flags](const std::vector<std::any>&) -> std::any {
                    std::string cmd = cxx + " " + flags + " -c " + src + " -o " + obj_file;
                    std::cout << "[Builder] Node \033[1;32m[" << orion::get_current_node_id() << "]\033[0m compiling " << src << " -> " << obj_file << "\n";
                    int ret = std::system(cmd.c_str());
                    if (ret != 0) {
                        std::cerr << "Compilation failed for " << src << "\n";
                        return -1; // Error
                    }
                    return 0; // Success
                }
            };

            object_refs.push_back(sched_.submit(compile_task));
        }

        orion::Task link_task{
            "link_" + target.name,
            object_refs,
            [target, object_files, cxx=target.cxx](const std::vector<std::any>& args) -> std::any {
                for (const auto& arg : args) {
                    if (std::any_cast<int>(arg) != 0) {
                        std::cerr << "[Builder] Cannot link due to previous compilation errors.\n";
                        return -1;
                    }
                }

                std::string cmd = cxx + " ";
                for (const auto& obj : object_files) {
                    cmd += obj + " ";
                }
                cmd += "-o " + target.name;

                std::cout << "[Builder] Node \033[1;34m[" << orion::get_current_node_id() << "]\033[0m linking -> " << target.name << "\n";
                int ret = std::system(cmd.c_str());
                if (ret != 0) {
                    std::cerr << "Linking failed for " << target.name << "\n";
                    return -1; // Error
                }
                return 0; // Success
            }
        };

        return sched_.submit(link_task);
    }

private:
    SchedulerType& sched_;
};

} // namespace orion::build_engine
