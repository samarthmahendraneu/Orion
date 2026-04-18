FROM ubuntu:24.04 as builder

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential cmake pkg-config clang git \
    libgrpc++-dev protobuf-compiler-grpc libprotobuf-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . /app

# Build Orion
RUN sed -i 's|/opt/homebrew/bin/grpc_cpp_plugin|/usr/bin/grpc_cpp_plugin|g' Makefile && \
    sed -i 's|/opt/homebrew/bin/protoc|/usr/bin/protoc|g' Makefile && \
    sed -i 's|/opt/homebrew/bin/pkg-config|pkg-config|g' Makefile && \
    sed -i 's|-L/opt/homebrew/Cellar/[^ ]* ||g' Makefile && \
    protoc -I=src/distributed/proto --cpp_out=src/distributed/generated --grpc_out=src/distributed/generated --plugin=protoc-gen-grpc=/usr/bin/grpc_cpp_plugin src/distributed/proto/orion.proto && \
    make clean && make head node orion_client \
    CXX=clang++ CXXFLAGS="-std=c++23 -O2"

# ----------------------------------------
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y libgrpc++-dev protobuf-compiler-grpc clang curl && rm -rf /var/lib/apt/lists/*

WORKDIR /orion

# Copy binaries
COPY --from=builder /app/head /orion/head
COPY --from=builder /app/node /orion/node
COPY --from=builder /app/orion_client /orion/orion_client

# Define the shared entrypoint path for all containers
RUN mkdir -p /orion-workspace
WORKDIR /orion-workspace

ENTRYPOINT ["/orion/head"]
