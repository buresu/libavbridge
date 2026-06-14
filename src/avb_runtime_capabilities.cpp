#include "avbridge.h"
#include "runtime/avb_runtime_capabilities_impl.hpp"

extern "C" {

avb_result avb_decoder_probe_runtime_capabilities(
    avb_backend backend,
    const char *path,
    avb_decoder_capabilities *out
) {
    return avb_runtime_probe_decoder_impl(backend, path, out);
}

avb_result avb_encoder_probe_runtime_capabilities(
    avb_backend backend,
    const char *path,
    avb_encoder_capabilities *out
) {
    return avb_runtime_probe_encoder_impl(backend, path, out);
}

} // extern "C"
