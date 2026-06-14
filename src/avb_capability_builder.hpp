#pragma once

#include "avb_capability_common.hpp"

namespace avb::runtime {

template <typename Capabilities>
void add_video_codec(
    Capabilities &out,
    avb_video_codec codec,
    detail::Container container) {
    if (detail::container_accepts_video(container, codec))
        detail::add_unique(out.video_codecs, out.video_codec_count, codec);
}

template <typename Capabilities>
void add_audio_codec(
    Capabilities &out,
    avb_audio_codec codec,
    detail::Container container) {
    if (detail::container_accepts_audio(container, codec))
        detail::add_unique(out.audio_codecs, out.audio_codec_count, codec);
}

template <typename Capabilities>
void add_audio_codec_unchecked(
    Capabilities &out,
    avb_audio_codec codec) {
    detail::add_unique(out.audio_codecs, out.audio_codec_count, codec);
}

template <typename Capabilities>
void add_common_video_codecs(
    Capabilities &out,
    detail::Container container) {
    add_video_codec(out, AVB_VIDEO_CODEC_H264, container);
    add_video_codec(out, AVB_VIDEO_CODEC_HEVC, container);
    add_video_codec(out, AVB_VIDEO_CODEC_VP8, container);
    add_video_codec(out, AVB_VIDEO_CODEC_VP9, container);
    add_video_codec(out, AVB_VIDEO_CODEC_AV1, container);
}

template <typename Capabilities>
void add_common_audio_codecs(
    Capabilities &out,
    detail::Container container) {
    add_audio_codec(out, AVB_AUDIO_CODEC_AAC, container);
    add_audio_codec(out, AVB_AUDIO_CODEC_OPUS, container);
    add_audio_codec(out, AVB_AUDIO_CODEC_MP3, container);
    add_audio_codec(out, AVB_AUDIO_CODEC_FLAC, container);
    add_audio_codec(out, AVB_AUDIO_CODEC_VORBIS, container);
    add_audio_codec(out, AVB_AUDIO_CODEC_PCM_S16, container);
    add_audio_codec(out, AVB_AUDIO_CODEC_PCM_F32, container);
}

inline void add_software_pixel_formats(avb_decoder_capabilities &out) {
    detail::add_unique(
        out.pixel_formats, out.pixel_format_count, AVB_PIXEL_FORMAT_BGRA8);
    detail::add_unique(
        out.pixel_formats, out.pixel_format_count, AVB_PIXEL_FORMAT_RGBA8);
    detail::add_unique(
        out.pixel_formats, out.pixel_format_count, AVB_PIXEL_FORMAT_NV12);
    detail::add_unique(
        out.pixel_formats, out.pixel_format_count, AVB_PIXEL_FORMAT_I420);
}

template <typename Capabilities>
void add_memory(Capabilities &out, avb_video_memory_type memory) {
    detail::add_unique(out.video_memory, out.video_memory_count, memory);
}

template <typename Capabilities>
void add_device(Capabilities &out, avb_hardware_device device) {
    detail::add_unique(
        out.hardware_devices, out.hardware_device_count, device);
}

}  // namespace avb::runtime
