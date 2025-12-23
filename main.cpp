//
// Created by Samarth Mahendra on 12/21/25.
//

#include "worker.h"
#include "task.h"
#include "object_store.h"

#include <iostream>
#include <any>        // ✅ REQUIRED for std::any_cast
#include <string>     // ✅ fixed typo


int main(){
    orion::ObjectStore store;
    orion::Worker worker(store);

    orion::Task task{
        "task-1",
        []() -> std::any {
            return 21 * 2;
        }
    };

    // Submit task
    orion::ObjectRef ref = worker.submit(task);

    // Execute task (in real systems, this would be another thread/process)
    worker.run();

    // BLOCK until result exists
    std::any result = store.get_blocking(ref.id);

    std::cout << "Result: "
              << std::any_cast<int>(result)
              << std::endl;

    return 0;

}