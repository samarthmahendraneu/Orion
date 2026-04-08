CXX := clang++

# ─────────────────────────────────────────────
# Build modes
# ─────────────────────────────────────────────
BASE_FLAGS := -std=c++23 -Wall -Wextra -pthread
RELEASE_FLAGS := -O2
DEBUG_FLAGS := -O0 -g
ASAN_FLAGS := -fsanitize=address -fno-omit-frame-pointer

# Default build = release
CXXFLAGS := $(BASE_FLAGS) $(RELEASE_FLAGS)
LDFLAGS :=

# ─────────────────────────────────────────────
# gRPC / Protobuf
# ─────────────────────────────────────────────
export PKG_CONFIG_PATH := /opt/homebrew/opt/grpc/lib/pkgconfig:/opt/homebrew/opt/protobuf/lib/pkgconfig:/opt/homebrew/opt/abseil/lib/pkgconfig

SRC := src
GEN_DIR := $(SRC)/distributed/generated

GRPC_INC := $(shell pkg-config --cflags grpc++ protobuf) -I$(SRC)
GRPC_LIB := $(shell pkg-config --libs grpc++ protobuf)

# ─────────────────────────────────────────────
# Generated proto files
# ─────────────────────────────────────────────
GEN_SRCS := \
	$(GEN_DIR)/orion.pb.cc \
	$(GEN_DIR)/orion.grpc.pb.cc

GEN_OBJS := $(GEN_SRCS:.cc=.o)

# ─────────────────────────────────────────────
# Source groups
# ─────────────────────────────────────────────
CORE_SRCS := \
	$(SRC)/core/context.cpp \
	$(SRC)/core/worker.cpp \
	$(SRC)/core/object_store.cpp \
	$(SRC)/core/scheduler.cpp \
	$(SRC)/local/runtime.cpp

CLUSTER_SRCS := \
	$(SRC)/distributed/cluster/cluster_scheduler.cpp \
	$(SRC)/distributed/cluster/node_registry.cpp

FUNC_SRCS := \
	$(SRC)/distributed/functions/function_registry.cpp

NODE_RT_SRC := $(SRC)/distributed/node_runtime.cpp

MAIN_SRCS := $(SRC)/main.cpp $(CORE_SRCS) $(CLUSTER_SRCS) $(NODE_RT_SRC)
HEAD_SRCS := $(SRC)/head_main.cpp $(CORE_SRCS) $(CLUSTER_SRCS) $(NODE_RT_SRC)
NODE_SRCS := $(SRC)/node_main.cpp $(CORE_SRCS) $(NODE_RT_SRC) $(FUNC_SRCS)
SUBMIT_SRCS := benchmarks/simple_task_test.cpp
SUBMIT_BENCHMARK_SRCS := benchmarks/compiler_wide_dag.cpp
TEST_PROJECT_BENCHMARK_SRCS := benchmarks/test_project_dag.cpp
UNIVERSAL_BUILDER_SRCS := benchmarks/universal_cmake_orchestrator.cpp

MAIN_OBJS := $(MAIN_SRCS:.cpp=.o)
HEAD_OBJS := $(HEAD_SRCS:.cpp=.o)
NODE_OBJS := $(NODE_SRCS:.cpp=.o)
SUBMIT_OBJS := $(SUBMIT_SRCS:.cpp=.o)
SUBMIT_BENCHMARK_OBJS := $(SUBMIT_BENCHMARK_SRCS:.cpp=.o)
TEST_PROJECT_BENCHMARK_OBJS := $(TEST_PROJECT_BENCHMARK_SRCS:.cpp=.o)
UNIVERSAL_BUILDER_OBJS := $(UNIVERSAL_BUILDER_SRCS:.cpp=.o)

BUILD_ENGINE_SRCS :=

BUILD_TEST_SRCS := build_test.cpp $(CORE_SRCS) $(BUILD_ENGINE_SRCS)
BUILD_TEST_OBJS := $(BUILD_TEST_SRCS:.cpp=.o)

BUILD_CLUSTER_TEST_SRCS := build_cluster_test.cpp $(CORE_SRCS) $(CLUSTER_SRCS) $(NODE_RT_SRC) $(BUILD_ENGINE_SRCS)
BUILD_CLUSTER_TEST_OBJS := $(BUILD_CLUSTER_TEST_SRCS:.cpp=.o)

BUILD_COMPLEX_TEST_SRCS := build_complex_test.cpp $(CORE_SRCS) $(CLUSTER_SRCS) $(NODE_RT_SRC) $(BUILD_ENGINE_SRCS)
BUILD_COMPLEX_TEST_OBJS := $(BUILD_COMPLEX_TEST_SRCS:.cpp=.o)

BENCHMARK_TEST_SRCS := benchmark_test.cpp $(CORE_SRCS) $(CLUSTER_SRCS) $(NODE_RT_SRC) $(BUILD_ENGINE_SRCS)
BENCHMARK_TEST_OBJS := $(BENCHMARK_TEST_SRCS:.cpp=.o)

# ─────────────────────────────────────────────
# Targets
# ─────────────────────────────────────────────
main: $(MAIN_OBJS) $(GEN_OBJS)
	$(CXX) $(CXXFLAGS) $^ $(GRPC_LIB) $(LDFLAGS) -o main

head: $(HEAD_OBJS) $(GEN_OBJS)
	$(CXX) $(CXXFLAGS) $^ $(GRPC_LIB) $(LDFLAGS) -o head

node: $(NODE_OBJS) $(GEN_OBJS)
	$(CXX) $(CXXFLAGS) $^ $(GRPC_LIB) $(LDFLAGS) -o node

submit_test: $(SUBMIT_OBJS) $(GEN_OBJS)
	$(CXX) $(CXXFLAGS) $^ $(GRPC_LIB) $(LDFLAGS) -o submit_test

submit_benchmark: $(SUBMIT_BENCHMARK_OBJS) $(GEN_OBJS)
	$(CXX) $(CXXFLAGS) $^ $(GRPC_LIB) $(LDFLAGS) -o submit_benchmark

build_test: $(BUILD_TEST_OBJS) $(GEN_OBJS)
	$(CXX) $(CXXFLAGS) $^ $(GRPC_LIB) $(LDFLAGS) -o build_test

build_cluster_test: $(BUILD_CLUSTER_TEST_OBJS) $(GEN_OBJS)
	$(CXX) $(CXXFLAGS) $^ $(GRPC_LIB) $(LDFLAGS) -o build_cluster_test

build_complex_test: $(BUILD_COMPLEX_TEST_OBJS) $(GEN_OBJS)
	$(CXX) $(CXXFLAGS) $^ $(GRPC_LIB) $(LDFLAGS) -o build_complex_test

benchmark_test: $(BENCHMARK_TEST_OBJS) $(GEN_OBJS)
	$(CXX) $(CXXFLAGS) $^ $(GRPC_LIB) $(LDFLAGS) -o benchmark_test

test_project_benchmark: $(TEST_PROJECT_BENCHMARK_OBJS) $(GEN_OBJS)
	$(CXX) $(CXXFLAGS) $^ $(GRPC_LIB) $(LDFLAGS) -o test_project_benchmark

universal_builder: $(UNIVERSAL_BUILDER_OBJS) $(GEN_OBJS)
	$(CXX) $(CXXFLAGS) $^ $(GRPC_LIB) $(LDFLAGS) -o universal_builder

# ─────────────────────────────────────────────
# Debug builds
# ─────────────────────────────────────────────
main_debug:
	$(MAKE) main CXXFLAGS="$(BASE_FLAGS) $(DEBUG_FLAGS)"

head_debug:
	$(MAKE) head CXXFLAGS="$(BASE_FLAGS) $(DEBUG_FLAGS)"

node_debug:
	$(MAKE) node CXXFLAGS="$(BASE_FLAGS) $(DEBUG_FLAGS)"

# ─────────────────────────────────────────────
# AddressSanitizer builds
# ─────────────────────────────────────────────
main_asan:
	$(MAKE) main \
	CXXFLAGS="$(BASE_FLAGS) $(DEBUG_FLAGS) $(ASAN_FLAGS)" \
	LDFLAGS="$(ASAN_FLAGS)"

head_asan:
	$(MAKE) head \
	CXXFLAGS="$(BASE_FLAGS) $(DEBUG_FLAGS) $(ASAN_FLAGS)" \
	LDFLAGS="$(ASAN_FLAGS)"

node_asan:
	$(MAKE) node \
	CXXFLAGS="$(BASE_FLAGS) $(DEBUG_FLAGS) $(ASAN_FLAGS)" \
	LDFLAGS="$(ASAN_FLAGS)"

# ─────────────────────────────────────────────
# Compile rules
# ─────────────────────────────────────────────
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(GRPC_INC) -MMD -MP -c $< -o $@

%.o: %.cc
	$(CXX) $(CXXFLAGS) $(GRPC_INC) -MMD -MP -c $< -o $@

# auto deps
-include $(MAIN_OBJS:.o=.d)
-include $(HEAD_OBJS:.o=.d)
-include $(NODE_OBJS:.o=.d)
-include $(SUBMIT_OBJS:.o=.d)
-include $(GEN_OBJS:.o=.d)

# ─────────────────────────────────────────────
# Clean
# ─────────────────────────────────────────────
clean:
	rm -f $(SRC)/**/*.o $(SRC)/**/*.d $(SRC)/*.o $(SRC)/*.d benchmarks/*.o benchmarks/*.d *.o *.d main head node submit_test submit_benchmark test_project_benchmark universal_builder build_test build_cluster_test build_complex_test 2>/dev/null || true

.PHONY: main head node submit_test submit_benchmark test_project_benchmark build_test build_cluster_test build_complex_test clean \
	main_debug head_debug node_debug \
	main_asan head_asan node_asan
