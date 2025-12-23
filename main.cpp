//
// Created by Samarth Mahendra on 12/21/25.
//

#include "worker.h"
#include "task.h"
#include "object_store.h"

#include <iostream>
#include <any>        // ✅ REQUIRED for std::any_cast
#include <string>     // ✅ fixed typo
#include "scheduler.h"


int main(){
    orion::ObjectStore store;
    orion::Worker w1(store);
    orion::Worker w2(store);
    orion::Scheduler scheduler({&w1, &w2}, store);

    orion::Task t1{
        "A",
        {},
        [] { return std::any(10); }
    };

    orion::Task t2{
        "B",
        { orion::ObjectRef{"A"} },
        [] { return std::any(20); }
    };

    scheduler.submit(t1);
    scheduler.submit(t2);

    // Initial schedule to dispatch ready tasks
    scheduler.schedule();

    // Run tasks - notifications happen automatically via ObjectStore callback
    w1.run();   // runs A → store.put() → scheduler notified → B becomes ready
    w2.run();   // runs B

    std::cout << std::any_cast<int>(store.get_blocking("A")) << std::endl;
    std::cout << std::any_cast<int>(store.get_blocking("B")) << std::endl;

    return 0;
}