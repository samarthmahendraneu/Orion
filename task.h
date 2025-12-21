#pragma once
#ifndef TASK_H
#define TASK_H

#endif //TASK_H

#include <string>
#include <functional>
#include <any>



// PURPOSE:
// --------
// Defines the fundamental unit of execution in Orion.
// A Task represents "something that can be executed by a worker".
//
// This is the smallest abstraction in the system.
// Everything else (scheduler, workers, object store) exists to move Tasks around.
//
// ----------------------------
// 1. A unique identifier for each task (TaskId).
// 2. A way to represent executable work (initially a callable).
// 3. (Later) Metadata such as dependencies, status, or resource hints.
//
// - Keep this simple.
// - Start with a fixed return type (e.g., int or std::any).
// - This file must NOT know about workers or schedulers.

namespace orion {

    // TODO:
    // - Define TaskId type
    // - Define Task struct/class
    // - Store a callable representing the work
    // - Store the task's ID
    struct Task {
        std::string id;
        std::function<std::any()> work;
    };

}

