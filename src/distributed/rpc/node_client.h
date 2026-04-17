//
// Created by Samarth Mahendra on 2/27/26.
//

#ifndef NODE_CLIENT_H
#define NODE_CLIENT_H

#pragma once

#include <string>
#include <memory>

#include "../../core/task.h"
#include "../../core/object_ref.h"

namespace orion::distributed {

    // Abstract client: "send a task to a node".
    //
    // CONTRACT CHANGE (Reliability hardening):
    // Returns `true` if the dispatch RPC was successfully accepted by the
    // target node, `false` otherwise (unknown node, dead channel, RPC error,
    // server rejected the request).  The caller MUST check the return value
    // and take corrective action (e.g. re-queue the task, pick a different
    // node) — otherwise a dispatch failure silently drops the task from the
    // DAG and any downstream tasks will wait forever.
    //
    // The output ObjectRef is trivially `ObjectRef{task.id}`; callers that
    // need it can construct it themselves, so we no longer pretend to return
    // a success-only ObjectRef from a function that can legitimately fail.
    class NodeClient {
    public:
        virtual ~NodeClient() = default;

        [[nodiscard]]
        virtual bool submit_task(const std::string& node_id,
                                 orion::Task task) = 0;
    };

} // namespace orion::distributed

#endif //NODE_CLIENT_H
