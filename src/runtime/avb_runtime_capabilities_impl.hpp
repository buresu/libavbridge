#pragma once

#include "avbridge.h"

extern "C" {

avb_result avb_runtime_probe_decoder_impl(
    avb_backend backend,
    const char *path,
    avb_decoder_capabilities *out);

avb_result avb_runtime_probe_encoder_impl(
    avb_backend backend,
    const char *path,
    avb_encoder_capabilities *out);

}
