#pragma once

#include "avbridge.h"
#include "avb_backend.hpp"
#include <memory>
#include <string>

struct avb_decoder {
    std::unique_ptr<AvbBackend> backend;
    std::string last_error;

    // Cached at open for the C-layer conveniences (seek clamp, EOF distinction).
    double duration_sec   = 0.0;
    bool   audio_available = false;
    bool   audio_eof       = false;

    // Heap-owned context backing avb_decoder_open_memory's I/O callbacks; freed
    // on close. Null for file/path opens.
    void  *mem_io = nullptr;

    void set_error(const char *msg) {
        last_error = msg ? msg : "";
    }
};
