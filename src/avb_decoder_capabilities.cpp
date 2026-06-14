#include "avbridge.h"
#include "avb_capability_builder.hpp"
#include "avb_capability_common.hpp"

namespace {

using avb::detail::Container;
using avb::detail::audio_only_container;
using avb::detail::container_from_path;
using avb::detail::container_name;
using avb::detail::resolve_backend;
using avb::runtime::add_audio_codec;
using avb::runtime::add_common_audio_codecs;
using avb::runtime::add_common_video_codecs;
using avb::runtime::add_device;
using avb::runtime::add_memory;
using avb::runtime::add_software_pixel_formats;
using avb::runtime::add_video_codec;

static void fill_ffmpeg(avb_decoder_capabilities &out, Container c) {
    if (!audio_only_container(c)) add_common_video_codecs(out, c);
    add_common_audio_codecs(out, c);
    add_software_pixel_formats(out);

    add_memory(out, AVB_VIDEO_MEMORY_CPU);
    add_memory(out, AVB_VIDEO_MEMORY_NATIVE);
#if defined(__linux__)
    add_memory(out, AVB_VIDEO_MEMORY_DMABUF);
#endif

    add_device(out, AVB_HW_DEVICE_AUTO);
    add_device(out, AVB_HW_DEVICE_VAAPI);
    add_device(out, AVB_HW_DEVICE_CUDA);
    add_device(out, AVB_HW_DEVICE_QSV);
    add_device(out, AVB_HW_DEVICE_D3D11VA);
    add_device(out, AVB_HW_DEVICE_VIDEOTOOLBOX);
}

static void fill_gstreamer(avb_decoder_capabilities &out, Container c) {
    if (!audio_only_container(c)) add_common_video_codecs(out, c);
    add_common_audio_codecs(out, c);
    add_software_pixel_formats(out);

    add_memory(out, AVB_VIDEO_MEMORY_CPU);
    add_memory(out, AVB_VIDEO_MEMORY_NATIVE);
#if defined(__linux__)
    add_memory(out, AVB_VIDEO_MEMORY_DMABUF);
#endif

    add_device(out, AVB_HW_DEVICE_AUTO);
    add_device(out, AVB_HW_DEVICE_VAAPI);
}

static void fill_platform(avb_decoder_capabilities &out, Container c) {
    if (!audio_only_container(c)) {
        add_video_codec(out, AVB_VIDEO_CODEC_H264, c);
        add_video_codec(out, AVB_VIDEO_CODEC_HEVC, c);
    }
    add_audio_codec(out, AVB_AUDIO_CODEC_AAC, c);
    add_audio_codec(out, AVB_AUDIO_CODEC_MP3, c);
    add_software_pixel_formats(out);
    add_memory(out, AVB_VIDEO_MEMORY_CPU);
    if (!audio_only_container(c))
        add_memory(out, AVB_VIDEO_MEMORY_NATIVE);
    add_device(out, AVB_HW_DEVICE_AUTO);
    if (!audio_only_container(c))
        add_device(out, AVB_HW_DEVICE_VIDEOTOOLBOX);
}

static void fill_mediafoundation(avb_decoder_capabilities &out, Container c) {
    if (c == Container::ivf) {
        add_video_codec(out, AVB_VIDEO_CODEC_VP8, c);
        add_video_codec(out, AVB_VIDEO_CODEC_VP9, c);
        add_video_codec(out, AVB_VIDEO_CODEC_AV1, c);
        add_software_pixel_formats(out);
        add_memory(out, AVB_VIDEO_MEMORY_CPU);
        add_memory(out, AVB_VIDEO_MEMORY_NATIVE);
        add_device(out, AVB_HW_DEVICE_AUTO);
        add_device(out, AVB_HW_DEVICE_D3D11VA);
        return;
    }
    if (!audio_only_container(c)) {
        add_common_video_codecs(out, c);
    }
    add_common_audio_codecs(out, c);
    add_software_pixel_formats(out);
    add_memory(out, AVB_VIDEO_MEMORY_CPU);
    if (!audio_only_container(c))
        add_memory(out, AVB_VIDEO_MEMORY_NATIVE);
    add_device(out, AVB_HW_DEVICE_AUTO);
    if (!audio_only_container(c))
        add_device(out, AVB_HW_DEVICE_D3D11VA);
}

} // namespace

extern "C" {

avb_result avb_decoder_query_capabilities(avb_backend backend, const char *path,
                                          avb_decoder_capabilities *out) {
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
            fill_ffmpeg(*out, container);
            break;
        case AVB_BACKEND_GSTREAMER:
            fill_gstreamer(*out, container);
            break;
        case AVB_BACKEND_MEDIAFOUNDATION:
            fill_mediafoundation(*out, container);
            break;
        case AVB_BACKEND_AVFOUNDATION:
            fill_platform(*out, container);
            break;
        default:
            out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
            out->message = "Requested decoder backend is not available.";
            return AVB_OK;
    }

    out->can_decode_video = out->video_codec_count > 0 ? 1 : 0;
    out->can_decode_audio = out->audio_codec_count > 0 ? 1 : 0;
    out->message = "Decoder capabilities are statically available.";
    return AVB_OK;
}

} // extern "C"
