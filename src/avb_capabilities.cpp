#include "avbridge.h"
#include "avb_capability_query.hpp"

using avb::detail::Container;
using avb::detail::audio_only_container;
using avb::capability::add_audio_codec;
using avb::capability::add_audio_codec_unchecked;
using avb::capability::add_common_audio_codecs;
using avb::capability::add_common_video_codecs;
using avb::capability::add_device;
using avb::capability::add_memory;
using avb::capability::add_software_pixel_formats;
using avb::capability::add_video_codec;
using avb::capability::QueryTraits;
using avb::capability::run_query;

namespace {

template <typename Capabilities>
void add_portable_capabilities(Capabilities &out, Container container) {
    if (!audio_only_container(container))
        add_common_video_codecs(out, container);
    add_common_audio_codecs(out, container);
    add_memory(out, AVB_VIDEO_MEMORY_CPU);
    add_memory(out, AVB_VIDEO_MEMORY_NATIVE);
#if defined(__linux__)
    add_memory(out, AVB_VIDEO_MEMORY_DMABUF);
#endif
    add_device(out, AVB_HW_DEVICE_AUTO);
    add_device(out, AVB_HW_DEVICE_VAAPI);
}

void fill_ffmpeg(avb_decoder_capabilities &out, Container container) {
    add_portable_capabilities(out, container);
    add_software_pixel_formats(out);
    add_device(out, AVB_HW_DEVICE_CUDA);
    add_device(out, AVB_HW_DEVICE_QSV);
    add_device(out, AVB_HW_DEVICE_D3D11VA);
    add_device(out, AVB_HW_DEVICE_VIDEOTOOLBOX);
}

void fill_ffmpeg(avb_encoder_capabilities &out, Container container) {
    add_portable_capabilities(out, container);
    add_device(out, AVB_HW_DEVICE_CUDA);
    add_device(out, AVB_HW_DEVICE_QSV);
    add_device(out, AVB_HW_DEVICE_D3D11VA);
    add_device(out, AVB_HW_DEVICE_VIDEOTOOLBOX);
    add_device(out, AVB_HW_DEVICE_AMF);
}

void fill_gstreamer(avb_decoder_capabilities &out, Container container) {
    add_portable_capabilities(out, container);
    add_software_pixel_formats(out);
}

void fill_gstreamer(avb_encoder_capabilities &out, Container container) {
    add_portable_capabilities(out, container);
}

void fill_avfoundation(
    avb_decoder_capabilities &out,
    Container container) {
    if (!audio_only_container(container)) {
        add_video_codec(out, AVB_VIDEO_CODEC_H264, container);
        add_video_codec(out, AVB_VIDEO_CODEC_HEVC, container);
    }
    add_audio_codec(out, AVB_AUDIO_CODEC_AAC, container);
    add_audio_codec(out, AVB_AUDIO_CODEC_MP3, container);
    add_software_pixel_formats(out);
    add_memory(out, AVB_VIDEO_MEMORY_CPU);
    if (!audio_only_container(container))
        add_memory(out, AVB_VIDEO_MEMORY_NATIVE);
    add_device(out, AVB_HW_DEVICE_AUTO);
    if (!audio_only_container(container))
        add_device(out, AVB_HW_DEVICE_VIDEOTOOLBOX);
}

void fill_avfoundation(
    avb_encoder_capabilities &out,
    Container container) {
    if (!audio_only_container(container)) {
        add_video_codec(out, AVB_VIDEO_CODEC_H264, container);
        add_video_codec(out, AVB_VIDEO_CODEC_HEVC, container);
    }
    add_audio_codec(out, AVB_AUDIO_CODEC_AAC, container);
    add_memory(out, AVB_VIDEO_MEMORY_CPU);
    if (!audio_only_container(container))
        add_memory(out, AVB_VIDEO_MEMORY_NATIVE);
    add_device(out, AVB_HW_DEVICE_AUTO);
    if (!audio_only_container(container))
        add_device(out, AVB_HW_DEVICE_VIDEOTOOLBOX);
}

void fill_mediafoundation(
    avb_decoder_capabilities &out,
    Container container) {
    if (container == Container::ivf) {
        add_video_codec(out, AVB_VIDEO_CODEC_VP8, container);
        add_video_codec(out, AVB_VIDEO_CODEC_VP9, container);
        add_video_codec(out, AVB_VIDEO_CODEC_AV1, container);
        add_software_pixel_formats(out);
        add_memory(out, AVB_VIDEO_MEMORY_CPU);
        add_memory(out, AVB_VIDEO_MEMORY_NATIVE);
        add_device(out, AVB_HW_DEVICE_AUTO);
        add_device(out, AVB_HW_DEVICE_D3D11VA);
        return;
    }

    if (!audio_only_container(container))
        add_common_video_codecs(out, container);
    add_common_audio_codecs(out, container);
    add_software_pixel_formats(out);
    add_memory(out, AVB_VIDEO_MEMORY_CPU);
    if (!audio_only_container(container))
        add_memory(out, AVB_VIDEO_MEMORY_NATIVE);
    add_device(out, AVB_HW_DEVICE_AUTO);
    if (!audio_only_container(container))
        add_device(out, AVB_HW_DEVICE_D3D11VA);
}

bool mediafoundation_video_container(Container container) {
    return container == Container::any ||
           container == Container::mp4 ||
           container == Container::mov ||
           container == Container::ivf ||
           container == Container::unknown;
}

void fill_mediafoundation(
    avb_encoder_capabilities &out,
    Container container) {
    if (mediafoundation_video_container(container)) {
        add_video_codec(out, AVB_VIDEO_CODEC_H264, container);
        add_video_codec(out, AVB_VIDEO_CODEC_HEVC, container);
        add_video_codec(out, AVB_VIDEO_CODEC_VP8, container);
        add_video_codec(out, AVB_VIDEO_CODEC_VP9, container);
        if (container == Container::any || container == Container::ivf)
            add_video_codec(out, AVB_VIDEO_CODEC_AV1, container);
    }

    switch (container) {
        case Container::any:
            add_audio_codec_unchecked(out, AVB_AUDIO_CODEC_AAC);
            add_audio_codec_unchecked(out, AVB_AUDIO_CODEC_MP3);
            add_audio_codec_unchecked(out, AVB_AUDIO_CODEC_FLAC);
            add_audio_codec_unchecked(out, AVB_AUDIO_CODEC_PCM_S16);
            add_audio_codec_unchecked(out, AVB_AUDIO_CODEC_PCM_F32);
            break;
        case Container::mp4:
        case Container::mov:
        case Container::m4a:
        case Container::unknown:
            add_audio_codec_unchecked(out, AVB_AUDIO_CODEC_AAC);
            break;
        case Container::mp3:
            add_audio_codec_unchecked(out, AVB_AUDIO_CODEC_MP3);
            break;
        case Container::flac:
            add_audio_codec_unchecked(out, AVB_AUDIO_CODEC_FLAC);
            break;
        case Container::wav:
            add_audio_codec_unchecked(out, AVB_AUDIO_CODEC_PCM_S16);
            add_audio_codec_unchecked(out, AVB_AUDIO_CODEC_PCM_F32);
            break;
        default:
            break;
    }

    add_memory(out, AVB_VIDEO_MEMORY_CPU);
    if (container == Container::any ||
        container == Container::ivf ||
        container == Container::mp4 ||
        container == Container::mov) {
        add_memory(out, AVB_VIDEO_MEMORY_NATIVE);
    }
    add_device(out, AVB_HW_DEVICE_AUTO);
    if (container == Container::any ||
        container == Container::ivf ||
        container == Container::mp4 ||
        container == Container::mov) {
        add_device(out, AVB_HW_DEVICE_D3D11VA);
    }
}

template <typename Capabilities>
bool fill_backend(
    avb_backend backend,
    Capabilities &out,
    Container container) {
    switch (backend) {
        case AVB_BACKEND_FFMPEG:
            fill_ffmpeg(out, container);
            return true;
        case AVB_BACKEND_GSTREAMER:
            fill_gstreamer(out, container);
            return true;
        case AVB_BACKEND_MEDIAFOUNDATION:
            fill_mediafoundation(out, container);
            return true;
        case AVB_BACKEND_AVFOUNDATION:
            fill_avfoundation(out, container);
            return true;
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
