#pragma once
#include <string>
#include <vector>

namespace orion::build_engine {

struct BuildTarget {
    std::string name;
    std::vector<std::string> sources;
    std::string cxx = "clang++";
    std::string flags = "-std=c++23 -O2";
};

} // namespace orion::build_engine
