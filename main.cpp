#include "worker.h"
#include "task.h"
#include "scheduler.h"
#include "object_store.h"

#include <iostream>
#include <chrono>
#include <any>

using namespace std::chrono;

int main() {
    orion::ObjectStore store;

    orion::Worker w1(store);
    orion::Worker w2(store);

    orion::Scheduler scheduler({&w1, &w2}, store);

    // Start worker threads
    w1.start();
    w2.start();

    // Heavy CPU-bound task (~1 second busy loop)
    auto heavy_task = [](std::vector<std::any>) -> std::any {
        auto start = steady_clock::now();
        while (steady_clock::now() - start < seconds(1)) {
            // busy spin
        }
        return std::any(1);
    };

    orion::Task t1{
        "T1",
        {},
        heavy_task
    };

    orion::Task t2{
        "T2",
        {},
        heavy_task
    };

    auto t0 = steady_clock::now();

    scheduler.submit(t1);
    scheduler.submit(t2);
    scheduler.schedule();

    // Block until both complete
    store.get_blocking("T1");
    store.get_blocking("T2");

    auto t1_end = steady_clock::now();

    auto elapsed_ms =
        duration_cast<milliseconds>(t1_end - t0).count();

    std::cout << "Elapsed time: " << elapsed_ms << " ms\n";

    w1.stop();
    w2.stop();

    return 0;
}
