#include "context.h"

namespace orion {

    thread_local std::string tls_node_id = "local";

    void set_current_node_id(const std::string& node_id) {
        tls_node_id = node_id;
    }

    const std::string& get_current_node_id() {
        return tls_node_id;
    }

} // namespace orion
