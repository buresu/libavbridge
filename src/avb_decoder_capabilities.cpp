#include "avbridge.h"
#include "avb_capability_common.hpp"

namespace {

using avb::detail::Container;
using avb::detail::audio_only_container;
using avb::detail::container_accepts_audio;
using avb::detail::container_accepts_video;
using avb::detail::container_from_path;
using avb::detail::container_name;
using avb::detail::resolve_backend;

static void add_video_codec(avb_decoder_capabilities &out, avb_video_codec codec,
                            Container container) {
    if (!container_accepts_video(container, codec)) return;
    if (out.video_codec_count >= AVB_MAX_CODEC_CAPS) return;
    out.video_codecs[out.video_codec_count++] = codec;
}

static void add_audio_codec(avb_decoder_capabilities &out, avb_audio_codec codec,
                            Container container) {
    if (!container_accepts_audio(container, codec)) return;
    if (out.audio_codec_count >= AVB_MAX_CODEC_CAPS) return;
    out.audio_codecs[out.audio_codec_count++] = codec;
}

static void add_pixel_format(avb_decoder_capabilities &out,
                             avb_pixel_format format) {
    if (out.pixel_format_count >= AVB_MAX_PIXEL_FORMAT_CAPS) return;
    out.pixel_formats[out.pixel_format_count++] = format;
}

static void add_memory(avb_decoder_capabilities &out,
                       avb_video_memory_type memory) {
    if (out.video_memory_count >= AVB_MAX_VIDEO_MEMORY_CAPS) return;
    out.video_memory[out.video_memory_count++] = memory;
}

static void add_device(avb_decoder_capabilities &out, avb_hardware_device device) {
    if (out.hardware_device_count >= AVB_MAX_HARDWARE_DEVICE_CAPS) return;
    out.hardware_devices[out.hardware_device_count++] = device;
}

static void add_common_video_codecs(avb_decoder_capabilities &out, Container c) {
    add_video_codec(out, AVB_VIDEO_CODEC_H264, c);
    add_video_codec(out, AVB_VIDEO_CODEC_HEVC, c);
    add_video_codec(out, AVB_VIDEO_CODEC_VP8, c);
    add_video_codec(out, AVB_VIDEO_CODEC_VP9, c);
    add_video_codec(out, AVB_VIDEO_CODEC_AV1, c);
}

static void add_common_audio_codecs(avb_decoder_capabilities &out, Container c) {
    add_audio_codec(out, AVB_AUDIO_CODEC_AAC, c);
    add_audio_codec(out, AVB_AUDIO_CODEC_OPUS, c);
    add_audio_codec(out, AVB_AUDIO_CODEC_MP3, c);
    add_audio_codec(out, AVB_AUDIO_CODEC_FLAC, c);
    add_audio_codec(out, AVB_AUDIO_CODEC_VORBIS, c);
    add_audio_codec(out, AVB_AUDIO_CODEC_PCM_S16, c);
    add_audio_codec(out, AVB_AUDIO_CODEC_PCM_F32, c);
}

static void add_software_pixel_formats(avb_decoder_capabilities &out) {
    add_pixel_format(out, AVB_PIXEL_FORMAT_BGRA8);
    add_pixel_format(out, AVB_PIXEL_FORMAT_RGBA8);
    add_pixel_format(out, AVB_PIXEL_FORMAT_NV12);
    add_pixel_format(out, AVB_PIXEL_FORMAT_I420);
}

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
    add_device(out, AVB_HW_DEVICE_AUTO);
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
        case AVB_BACKEND_AVFOUNDATION:
        case AVB_BACKEND_MEDIAFOUNDATION:
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
