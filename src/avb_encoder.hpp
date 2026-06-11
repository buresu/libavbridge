#pragma once

#include "avbridge.h"
#include "avb_encoder_impl.hpp"

#include <memory>
#include <string>

struct avb_encoder {
    std::unique_ptr<AvbEncoderImpl> impl;
    std::string last_error;

    void set_error(const char *msg) { last_error = msg ? msg : ""; }
};
