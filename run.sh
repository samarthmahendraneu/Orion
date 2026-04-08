#!/bin/bash
killall head node submit_benchmark 2>/dev/null || true
rm -f bench_util_* bench_main.cpp seq_obj_* dist_obj_* bench_exec_*

./head > /tmp/head.log 2>&1 &
H_PID=$!
sleep 1

./node 50050 50051 node-1 > /tmp/node1.log 2>&1 &
./node 50050 50052 node-2 > /tmp/node2.log 2>&1 &
./node 50050 50053 node-3 > /tmp/node3.log 2>&1 &
./node 50050 50054 node-4 > /tmp/node4.log 2>&1 &

sleep 2
./submit_benchmark
killall head node 2>/dev/null || true
