#!/bin/bash
mkdir -p input/sample_cmake_project/src input/sample_cmake_project/include/sample_cmake_project
rm -f input/sample_cmake_project/src/*.cpp input/sample_cmake_project/include/sample_cmake_project/*.h

# We will create 4 parallel CHAINS of 8 modules each (32 total).
# Each chain is deep (A -> B -> C -> D), and the 4 chains run in parallel.
CHAINS=4
DEPTH=8
ITERATIONS=2000000

# 1. Generate Headers and Sources for the chains
for c in $(seq 0 $((CHAINS-1)))
do
    for d in $(seq 0 $((DEPTH-1)))
    do
        ID="c${c}_d${d}"
        HEADER="input/sample_cmake_project/include/sample_cmake_project/util_${ID}.h"
        SOURCE="input/sample_cmake_project/src/util_${ID}.cpp"
        
        # Header
        echo "#pragma once" > $HEADER
        echo "long long compute_${ID}(long long v);" >> $HEADER
        
        # Source
        cat <<EOF > $SOURCE
#include "sample_cmake_project/util_${ID}.h"
$(if [ $d -gt 0 ]; then echo "#include \"sample_cmake_project/util_c${c}_d$((d-1)).h\""; fi)

long long compute_${ID}(long long v) {
    long long s = v;
    for (int j = 0; j < $ITERATIONS; ++j) {
        s += (j * j) % 1000000007;
        s ^= (s >> 3);
    }
    $(if [ $d -gt 0 ]; then echo "    return s + compute_c${c}_d$((d-1))(v);"; else echo "    return s;"; fi)
}
EOF
    done
done

# 2. Main app that uses the tail of each chain
cat <<EOF > input/sample_cmake_project/src/main.cpp
#include <iostream>
$(for c in $(seq 0 $((CHAINS-1))); do echo "#include \"sample_cmake_project/util_c${c}_d$((DEPTH-1)).h\""; done)

int main() {
    long long total = 0;
    $(for c in $(seq 0 $((CHAINS-1))); do echo "    total += compute_c${c}_d$((DEPTH-1))(100);"; done)
    std::cout << "Final Result: " << total % 10000 << std::endl;
    return 0;
}
EOF

# 3. CMakeLists.txt
echo "cmake_minimum_required(VERSION 3.10)" > input/sample_cmake_project/CMakeLists.txt
echo "project(sample_cmake_project CXX)" >> input/sample_cmake_project/CMakeLists.txt
echo "set(CMAKE_CXX_STANDARD 23)" >> input/sample_cmake_project/CMakeLists.txt
echo "include_directories(include)" >> input/sample_cmake_project/CMakeLists.txt
echo -n "add_executable(sample_app src/main.cpp" >> input/sample_cmake_project/CMakeLists.txt
find input/sample_cmake_project/src -name "util_*.cpp" | sed 's|input/sample_cmake_project/||' | xargs -n1 echo -n " " >> input/sample_cmake_project/CMakeLists.txt
echo ")" >> input/sample_cmake_project/CMakeLists.txt
