#include "avb_capabilities_mediafoundation.hpp"

using avb::capability::add_audio_codec_unchecked;
using avb::capability::add_common_audio_codecs;
using avb::capability::add_common_video_codecs;
using avb::capability::add_device;
using avb::capability::add_memory;
using avb::capability::add_software_pixel_formats;
using avb::capability::add_video_codec;
using avb::detail::audio_only_container;
using avb::detail::Container;

namespace {

bool video_container(Container container) {
    return container == Container::any ||
           container == Container::mp4 ||
           container == Container::mov ||
           container == Container::ivf ||
           container == Container::unknown;
}

bool native_video_container(Container container) {
    return container == Container::any ||
           container == Container::ivf ||
           container == Container::mp4 ||
           container == Container::mov;
}

}  // namespace

void avb_fill_mediafoundation_capabilities(
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

void avb_fill_mediafoundation_capabilities(
    avb_encoder_capabilities &out,
    Container container) {
    if (video_container(container)) {
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
    if (native_video_container(container))
        add_memory(out, AVB_VIDEO_MEMORY_NATIVE);
    add_device(out, AVB_HW_DEVICE_AUTO);
    if (native_video_container(container))
        add_device(out, AVB_HW_DEVICE_D3D11VA);
}
