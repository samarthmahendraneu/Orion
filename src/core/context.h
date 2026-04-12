#pragma once

#include <string>

namespace orion {

    // Sets the node ID for the current thread
    void set_current_node_id(const std::string& node_id);

    // Gets the node ID for the current thread
    const std::string& get_current_node_id();

} // namespace orion
