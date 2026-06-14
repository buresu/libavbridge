#include "avbridge.h"
#include "avb_capability_query.hpp"

#if defined(AVB_ENABLE_AVFOUNDATION)
#include "backends/avfoundation/avb_capabilities_avfoundation.hpp"
#endif

#if defined(AVB_ENABLE_FFMPEG)
#include "backends/ffmpeg/avb_capabilities_ffmpeg.hpp"
#endif

#if defined(AVB_ENABLE_GSTREAMER)
#include "backends/gstreamer/avb_capabilities_gstreamer.hpp"
#endif

#if defined(AVB_ENABLE_MEDIAFOUNDATION)
#include "backends/mediafoundation/avb_capabilities_mediafoundation.hpp"
#endif

using avb::capability::QueryTraits;
using avb::capability::run_query;
using avb::detail::Container;

namespace {

template <typename Capabilities>
bool fill_backend(
    avb_backend backend,
    Capabilities &out,
    Container container) {
    switch (backend) {
        case AVB_BACKEND_FFMPEG:
#if defined(AVB_ENABLE_FFMPEG)
            avb_fill_ffmpeg_capabilities(out, container);
            return true;
#else
            return false;
#endif
        case AVB_BACKEND_GSTREAMER:
#if defined(AVB_ENABLE_GSTREAMER)
            avb_fill_gstreamer_capabilities(out, container);
            return true;
#else
            return false;
#endif
        case AVB_BACKEND_MEDIAFOUNDATION:
#if defined(AVB_ENABLE_MEDIAFOUNDATION)
            avb_fill_mediafoundation_capabilities(out, container);
            return true;
#else
            return false;
#endif
        case AVB_BACKEND_AVFOUNDATION:
#if defined(AVB_ENABLE_AVFOUNDATION)
            avb_fill_avfoundation_capabilities(out, container);
            return true;
#else
            return false;
#endif
        default:
            return false;
    }
}

template <typename Capabilities>
avb_result query_capabilities(
    avb_backend backend,
    const char *path,
    Capabilities *out) {
    return run_query(
        backend,
        path,
        out,
        QueryTraits<Capabilities>::static_success_message,
        [](avb_backend resolved,
           Capabilities &caps,
           Container container,
           const char *&) {
            return fill_backend(resolved, caps, container);
        });
}

}  // namespace

extern "C" {

avb_result avb_decoder_query_capabilities(
    avb_backend backend,
    const char *path,
    avb_decoder_capabilities *out) {
    return query_capabilities(backend, path, out);
}

avb_result avb_encoder_query_capabilities(
    avb_backend backend,
    const char *path,
    avb_encoder_capabilities *out) {
    return query_capabilities(backend, path, out);
}

}  // extern "C"
