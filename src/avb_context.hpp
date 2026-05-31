#pragma once

#include "avbridge/avb.h"
#include "avb_backend.hpp"
#include <memory>
#include <string>

struct avb_context {
    std::unique_ptr<AvbBackend> backend;
    std::string last_error;

    void set_error(const char *msg) {
        last_error = msg ? msg : "";
    }
};
