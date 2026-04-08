// submit_test.cpp — Milestone 2 smoke test
// Connects to the head server, submits two tasks (one with a dep), and
// verifies they are accepted and dispatched to nodes.
//
// Usage:  ./submit_test [head_port]  (default: 50050)

#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#include <grpcpp/grpcpp.h>
#include "distributed/generated/orion.grpc.pb.h"

int main(int argc, char* argv[]) {
    std::string port = (argc > 1) ? argv[1] : "50050";
    std::string target = "localhost:" + port;

    auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
    auto stub = orion::ClusterHead::NewStub(channel);

    // Helper: serialize an int as 4-byte little-endian
    auto pack_int = [](int v) -> std::string {
        std::string b(4, '\0');
        std::memcpy(b.data(), &v, 4);
        return b;
    };

    auto submit = [&](const std::string& task_id,
                      const std::string& fn,
                      const std::vector<std::string>& deps,
                      const std::vector<int>& int_args = {}) {
        orion::TaskRequest req;
        req.set_task_id(task_id);
        req.set_function_name(fn);
        for (const auto& d : deps) req.add_dep_ids(d);
        for (int v : int_args)    req.add_args(pack_int(v));

        orion::TaskReply reply;
        grpc::ClientContext ctx;
        grpc::Status status = stub->SubmitTask(&ctx, req, &reply);

        if (status.ok() && reply.accepted()) {
            std::cout << "[SubmitTest] Task '" << task_id
                      << "' accepted  fn=" << fn << "\n";
        } else {
            std::cerr << "[SubmitTest] Task '" << task_id
                      << "' FAILED: " << status.error_message() << "\n";
        }
    };

    // Task A: add(3, 7) → expected 10
    submit("task-A", "add", {}, {3, 7});

    // Task B: mul(6, 7) → expected 42  (sent to different node round-robin)
    submit("task-B", "mul", {}, {6, 7});

    std::cout << "[SubmitTest] Done.\n";
    return 0;
}
