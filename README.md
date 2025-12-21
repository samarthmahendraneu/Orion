# Orion

A lightweight, thread-safe task execution framework in C++ for distributed computing.

## Overview

Orion is a task execution system designed to provide a simple abstraction for executing computational tasks. The framework is built around two core components: **Tasks** and **Workers**.

## Architecture

### Task (`task.h`)

The fundamental unit of execution in Orion. A Task represents work that can be executed by a worker.

```cpp
struct Task {
    std::string id;           // Unique identifier
    std::function<int()> work; // Executable work
};
```

### Worker (`worker.h`, `worker.cpp`)

An execution unit responsible for running tasks. Workers maintain a thread-safe queue and execute tasks on demand.

**Key Features:**
- Thread-safe task queue with mutex and condition variables
- Non-blocking task submission
- Synchronous task execution with `run()`
- Task inspection with `peek()`

**API:**
- `void submit(Task task)` - Enqueue a task for execution
- `std::optional<int> run()` - Execute the next available task
- `std::optional<Task> peek()` - View the next task without executing

## Usage

```cpp
#include "worker.h"
#include "task.h"
#include <iostream>

int main() {
    orion::Worker worker;

    // Create a task
    orion::Task task{
        "task-1",
        []() {
            return 21 * 2;
        }
    };

    // Submit and execute
    worker.submit(task);
    auto result = worker.run();

    if (result) {
        std::cout << "Result: " << *result << std::endl;
    }
}
```

## Building

This project uses C++17 or later. To build:

```bash
g++ -std=c++17 main.cpp worker.cpp -o main -pthread
./main
```

Or with clang:

```bash
clang++ -std=c++17 main.cpp worker.cpp -o main -pthread
./main
```

## Design Principles

1. **Separation of Concerns**: Workers execute tasks but don't make scheduling decisions
2. **Thread Safety**: All worker operations are protected by mutexes
3. **Simplicity**: Minimal abstraction with clear responsibilities
4. **Extensibility**: Designed to support future features like task dependencies and distributed execution

## Current Status

The project is in active development. Current features:
- Basic task definition and execution
- Thread-safe worker implementation
- Simple task queue management

## Future Roadmap

- Task scheduling and prioritization
- Task dependencies
- Multi-threaded worker pools
- Distributed task execution
- Object store integration
- Advanced metadata and resource hints

## License

MIT License

## Author

Samarth Mahendra