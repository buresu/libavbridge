#include "avb_capabilities_avfoundation.hpp"

using avb::capability::add_audio_codec;
using avb::capability::add_device;
using avb::capability::add_memory;
using avb::capability::add_software_pixel_formats;
using avb::capability::add_video_codec;
using avb::detail::audio_only_container;

void avb_fill_avfoundation_capabilities(
    avb_decoder_capabilities &out,
    avb::detail::Container container) {
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

void avb_fill_avfoundation_capabilities(
    avb_encoder_capabilities &out,
    avb::detail::Container container) {
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
