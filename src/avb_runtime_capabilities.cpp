#include "avbridge.h"
#include "avb_capability_query.hpp"

#if defined(AVB_ENABLE_FFMPEG)
#include "backends/ffmpeg/avb_runtime_ffmpeg.hpp"
#endif

#if defined(AVB_ENABLE_GSTREAMER)
#include "backends/gstreamer/avb_runtime_gstreamer.hpp"
#endif

#if defined(AVB_ENABLE_MEDIAFOUNDATION)
#include "backends/mediafoundation/avb_runtime_mediafoundation.hpp"
#endif

#if defined(AVB_ENABLE_AVFOUNDATION)
#include "backends/avfoundation/avb_runtime_avfoundation.hpp"
#endif

using avb::detail::Container;
using avb::capability::QueryTraits;
using avb::capability::run_query;

namespace {

#if defined(AVB_ENABLE_FFMPEG)
bool probe_ffmpeg(avb_decoder_capabilities &out, Container container) {
    return avb_probe_ffmpeg_decoder(out, container);
}

bool probe_ffmpeg(avb_encoder_capabilities &out, Container container) {
    return avb_probe_ffmpeg_encoder(out, container);
}
#endif

#if defined(AVB_ENABLE_GSTREAMER)
bool probe_gstreamer(avb_decoder_capabilities &out, Container container) {
    return avb_probe_gstreamer_decoder(out, container);
}

bool probe_gstreamer(avb_encoder_capabilities &out, Container container) {
    return avb_probe_gstreamer_encoder(out, container);
}
#endif

#if defined(AVB_ENABLE_MEDIAFOUNDATION)
bool probe_mediafoundation(
    avb_decoder_capabilities &out,
    Container container) {
    return avb_probe_mediafoundation_decoder(out, container);
}

bool probe_mediafoundation(
    avb_encoder_capabilities &out,
    Container container) {
    return avb_probe_mediafoundation_encoder(out, container);
}
#endif

#if defined(AVB_ENABLE_AVFOUNDATION)
bool probe_avfoundation(
    avb_decoder_capabilities &out,
    Container container) {
    return avb_probe_avfoundation_decoder(out, container);
}

bool probe_avfoundation(
    avb_encoder_capabilities &out,
    Container container) {
    return avb_probe_avfoundation_encoder(out, container);
}
#endif

template <typename Capabilities>
bool probe_backend(
    avb_backend backend,
    Capabilities &out,
    Container container,
    const char *&failure_message) {
    switch (backend) {
        case AVB_BACKEND_FFMPEG:
#if defined(AVB_ENABLE_FFMPEG)
            failure_message = "FFmpeg runtime libraries are not available.";
            return probe_ffmpeg(out, container);
#else
            return false;
#endif

        case AVB_BACKEND_GSTREAMER:
#if defined(AVB_ENABLE_GSTREAMER)
            failure_message =
                "GStreamer runtime libraries are not available.";
            return probe_gstreamer(out, container);
#else
            return false;
#endif

        case AVB_BACKEND_MEDIAFOUNDATION:
#if defined(AVB_ENABLE_MEDIAFOUNDATION)
            failure_message = "Media Foundation runtime is not available.";
            return probe_mediafoundation(out, container);
#else
            return false;
#endif

        case AVB_BACKEND_AVFOUNDATION:
#if defined(AVB_ENABLE_AVFOUNDATION)
            return probe_avfoundation(out, container);
#else
            return false;
#endif

        default:
            return false;
    }
}

template <typename Capabilities>
avb_result probe_runtime_capabilities(
    avb_backend backend,
    const char *path,
    Capabilities *out) {
    return run_query(
        backend,
        path,
        out,
        QueryTraits<Capabilities>::runtime_success_message,
        [](avb_backend resolved,
           Capabilities &caps,
           Container container,
           const char *&failure_message) {
            return probe_backend(
                resolved, caps, container, failure_message);
        });
}

}  // namespace

extern "C" {

avb_result avb_decoder_probe_runtime_capabilities(
    avb_backend backend,
    const char *path,
    avb_decoder_capabilities *out) {
    return probe_runtime_capabilities(backend, path, out);
}

avb_result avb_encoder_probe_runtime_capabilities(
    avb_backend backend,
    const char *path,
    avb_encoder_capabilities *out) {
    return probe_runtime_capabilities(backend, path, out);
}

}  // extern "C"
