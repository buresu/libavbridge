#include "avbridge.h"
#include "avb_capability_common.hpp"

#if defined(AVB_ENABLE_FFMPEG)
#include "avb_runtime_ffmpeg.hpp"
#endif

#if defined(AVB_ENABLE_GSTREAMER)
#include "avb_runtime_gstreamer.hpp"
#endif

#if defined(AVB_ENABLE_MEDIAFOUNDATION)
#include "avb_runtime_mediafoundation.hpp"
#endif

#if defined(AVB_ENABLE_AVFOUNDATION)
#include "avb_runtime_avfoundation.hpp"
#endif

using avb::detail::Container;
using avb::detail::container_from_path;
using avb::detail::container_name;
using avb::detail::resolve_backend;

extern "C" {

avb_result avb_runtime_probe_decoder_impl(
    avb_backend backend,
    const char *path,
    avb_decoder_capabilities *out
) {
    if (!out) return AVB_ERROR_INVALID_ARGUMENT;

    *out = {};
    Container container = container_from_path(path, Container::any);
    out->result = AVB_OK;
    out->backend = resolve_backend(backend);
    out->backend_name = avb_backend_name(out->backend);
    out->container_name = container_name(container);

    if (!out->backend_name || !avb_backend_is_available(out->backend)) {
        out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
        out->message = "Requested decoder backend is not available in this build.";
        return AVB_OK;
    }

    switch (out->backend) {
        case AVB_BACKEND_FFMPEG:
#if defined(AVB_ENABLE_FFMPEG)
        {
            if (!avb_probe_ffmpeg_decoder(*out, container)) {
                out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
                out->message = "FFmpeg runtime libraries are not available.";
                return AVB_OK;
            }
            break;
        }
#else
            out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
            out->message = "FFmpeg backend is not built.";
            return AVB_OK;
#endif

        case AVB_BACKEND_GSTREAMER:
#if defined(AVB_ENABLE_GSTREAMER)
        {
            if (!avb_probe_gstreamer_decoder(*out, container)) {
                out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
                out->message = "GStreamer runtime libraries are not available.";
                return AVB_OK;
            }
            break;
        }
#else
            out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
            out->message = "GStreamer backend is not built.";
            return AVB_OK;
#endif

        case AVB_BACKEND_MEDIAFOUNDATION:
#if defined(AVB_ENABLE_MEDIAFOUNDATION)
        {
            if (!avb_probe_mediafoundation_decoder(*out, container)) {
                out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
                out->message = "Media Foundation runtime is not available.";
                return AVB_OK;
            }
            break;
        }
#else
            out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
            out->message = "Media Foundation backend is not built.";
            return AVB_OK;
#endif

        case AVB_BACKEND_AVFOUNDATION:
#if defined(AVB_ENABLE_AVFOUNDATION)
            avb_probe_avfoundation_decoder(*out, container);
#else
            out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
            out->message = "AVFoundation backend is not built.";
            return AVB_OK;
#endif
            break;

        default:
            out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
            out->message = "Requested decoder backend is not available.";
            return AVB_OK;
    }

    out->can_decode_video = out->video_codec_count > 0 ? 1 : 0;
    out->can_decode_audio = out->audio_codec_count > 0 ? 1 : 0;
    out->message = "Decoder capabilities are available in the current runtime.";
    return AVB_OK;
}

avb_result avb_runtime_probe_encoder_impl(
    avb_backend backend,
    const char *path,
    avb_encoder_capabilities *out
) {
    if (!out) return AVB_ERROR_INVALID_ARGUMENT;

    *out = {};
    Container container = container_from_path(path, Container::any);
    out->result = AVB_OK;
    out->backend = resolve_backend(backend);
    out->backend_name = avb_backend_name(out->backend);
    out->container_name = container_name(container);

    if (!out->backend_name || !avb_backend_is_available(out->backend)) {
        out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
        out->message = "Requested encoder backend is not available in this build.";
        return AVB_OK;
    }

    switch (out->backend) {
        case AVB_BACKEND_FFMPEG:
#if defined(AVB_ENABLE_FFMPEG)
        {
            if (!avb_probe_ffmpeg_encoder(*out, container)) {
                out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
                out->message = "FFmpeg runtime libraries are not available.";
                return AVB_OK;
            }
            break;
        }
#else
            out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
            out->message = "FFmpeg backend is not built.";
            return AVB_OK;
#endif

        case AVB_BACKEND_GSTREAMER:
#if defined(AVB_ENABLE_GSTREAMER)
        {
            if (!avb_probe_gstreamer_encoder(*out, container)) {
                out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
                out->message = "GStreamer runtime libraries are not available.";
                return AVB_OK;
            }
            break;
        }
#else
            out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
            out->message = "GStreamer backend is not built.";
            return AVB_OK;
#endif

        case AVB_BACKEND_MEDIAFOUNDATION:
#if defined(AVB_ENABLE_MEDIAFOUNDATION)
        {
            if (!avb_probe_mediafoundation_encoder(*out, container)) {
                out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
                out->message = "Media Foundation runtime is not available.";
                return AVB_OK;
            }
            break;
        }
#else
            out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
            out->message = "Media Foundation backend is not built.";
            return AVB_OK;
#endif

        case AVB_BACKEND_AVFOUNDATION:
#if defined(AVB_ENABLE_AVFOUNDATION)
            avb_probe_avfoundation_encoder(*out, container);
#else
            out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
            out->message = "AVFoundation backend is not built.";
            return AVB_OK;
#endif
            break;

        default:
            out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
            out->message = "Requested encoder backend is not available.";
            return AVB_OK;
    }

    out->can_encode_video = out->video_codec_count > 0 ? 1 : 0;
    out->can_encode_audio = out->audio_codec_count > 0 ? 1 : 0;
    out->message = "Encoder capabilities are available in the current runtime.";
    return AVB_OK;
}

} // extern "C"
