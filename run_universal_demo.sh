#!/bin/bash

# 1. Kill any existing nodes
killall head node submit_benchmark universal_builder 2>/dev/null

# 2. Start Head
./head > /tmp/head.log 2>&1 &
echo "[Demo] Started Head"
sleep 1

# 3. Start 4 Nodes
./node 50050 50051 node-1 > /tmp/node1.log 2>&1 &
./node 50050 50052 node-2 > /tmp/node2.log 2>&1 &
./node 50050 50053 node-3 > /tmp/node3.log 2>&1 &
./node 50050 50054 node-4 > /tmp/node4.log 2>&1 &
echo "[Demo] Started 4 Worker Nodes"
sleep 2

# 4. Run the Universal Build Orchestrator
# This program will build the 'input/sample_cmake_project' project
# which is entirely distinct from Orion and uses generic shell commands.
./universal_builder

# 5. Cleanup
killall head node
echo "[Demo] Finished"
