//
// Created by Samarth Mahendra on 12/21/25.
//


#include "worker.h"
#include <optional>
#include "task.h"
#include <iostream>
#include <any>


int main(){
    orion::Worker worker;


    // simple task
    orion::Task task1;
    orion::Task task{
        "task-1",
        []() {
            return 21 * 2;
        }
    };

    // enqueue the task to the worker
    worker.submit(task);
    auto result = worker.run();
    if (result) {
        std::cout << "Result: " << std::any_cast<int>(*result) << std::endl;




}




}