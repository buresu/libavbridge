#include "avbridge.h"
#include "avb_media_rules.hpp"

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
using avb::detail::container_from_path;
using avb::detail::container_name;
using avb::detail::resolve_backend;

namespace {

template <typename Capabilities>
struct ProbeTraits;

template <>
struct ProbeTraits<avb_decoder_capabilities> {
    static constexpr const char *unavailable_message =
        "Requested decoder backend is not available in this build.";
    static constexpr const char *success_message =
        "Decoder capabilities are available in the current runtime.";

    static void finish(avb_decoder_capabilities &out) {
        out.can_decode_video = out.video_codec_count > 0 ? 1 : 0;
        out.can_decode_audio = out.audio_codec_count > 0 ? 1 : 0;
    }
};

template <>
struct ProbeTraits<avb_encoder_capabilities> {
    static constexpr const char *unavailable_message =
        "Requested encoder backend is not available in this build.";
    static constexpr const char *success_message =
        "Encoder capabilities are available in the current runtime.";

    static void finish(avb_encoder_capabilities &out) {
        out.can_encode_video = out.video_codec_count > 0 ? 1 : 0;
        out.can_encode_audio = out.audio_codec_count > 0 ? 1 : 0;
    }
};

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
    if (!out) return AVB_ERROR_INVALID_ARGUMENT;

    *out = {};
    Container container = container_from_path(path, Container::any);
    out->result = AVB_OK;
    out->backend = resolve_backend(backend);
    out->backend_name = avb_backend_name(out->backend);
    out->container_name = container_name(container);

    if (!out->backend_name || !avb_backend_is_available(out->backend)) {
        out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
        out->message = ProbeTraits<Capabilities>::unavailable_message;
        return AVB_OK;
    }

    const char *failure_message =
        ProbeTraits<Capabilities>::unavailable_message;
    if (!probe_backend(out->backend, *out, container, failure_message)) {
        out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
        out->message = failure_message;
        return AVB_OK;
    }

    ProbeTraits<Capabilities>::finish(*out);
    out->message = ProbeTraits<Capabilities>::success_message;
    return AVB_OK;
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
