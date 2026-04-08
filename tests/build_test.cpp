#include "src/local/runtime.h"
#include "src/build_engine/builder.h"
#include <fstream>
#include <iostream>

int main() {
    // 1. Create dummy project files
    std::ofstream out_main("hello_main.cpp");
    out_main << "#include <iostream>\n"
             << "void print_msg();\n"
             << "int main() { print_msg(); return 0; }\n";
    out_main.close();

    std::ofstream out_utils("hello_utils.cpp");
    out_utils << "#include <iostream>\n"
              << "void print_msg() { std::cout << \"Hello from Orion Build Engine!\\n\"; }\n";
    out_utils.close();

    // 2. Setup Orion Runtime
    orion::Runtime rt(4); // 4 workers

    // 3. Setup BuildTarget and Builder
    orion::build_engine::BuildTarget target{
        "hello_app", // name
        {"hello_main.cpp", "hello_utils.cpp"}, // sources
        "clang++", // cxx
        "-std=c++23 -O2" // flags
    };

    orion::build_engine::Builder builder(rt);

    // 4. Submit build
    std::cout << "Starting build of target: " << target.name << "\n";
    orion::ObjectRef final_ref = builder.build(target);

    // 5. Wait for the final linking task
    rt.wait(final_ref);

    // 6. Check result
    int link_result = std::any_cast<int>(rt.get(final_ref));
    if (link_result == 0) {
        std::cout << "\nBuild succeeded! Executing hello_app:\n";
        std::cout << "--------------------------------------\n";
        int ret = std::system("./hello_app");
        std::cout << "--------------------------------------\n";
        if (ret != 0) {
            std::cerr << "Execution failed.\n";
            return 1;
        }
    } else {
        std::cerr << "Build failed.\n";
        return 1;
    }

    rt.shutdown();
    return 0;
}
