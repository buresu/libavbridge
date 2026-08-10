#pragma once

#include "avbridge.h"
#include "avb_backend.hpp"

#include <cctype>
#include <cstring>

namespace avb::detail {

enum class Container {
    any,
    mp4,
    mov,
    m4a,
    webm,
    mkv,
    ogg,
    wav,
    flac,
    mp3,
    ivf,
    unknown,
};

inline bool ends_with_ci(const char *path, const char *suffix) {
    if (!path) return false;
    size_t plen = std::strlen(path);
    size_t slen = std::strlen(suffix);
    if (plen < slen) return false;
    path += plen - slen;
    for (size_t i = 0; i < slen; ++i) {
        unsigned char a = (unsigned char)path[i];
        unsigned char b = (unsigned char)suffix[i];
        if ((char)std::tolower(a) != (char)std::tolower(b)) return false;
    }
    return true;
}

inline Container container_from_path(const char *path, Container fallback) {
    if (!path || path[0] == '\0') return fallback;
    if (ends_with_ci(path, ".mov"))  return Container::mov;
    if (ends_with_ci(path, ".m4a"))  return Container::m4a;
    if (ends_with_ci(path, ".webm")) return Container::webm;
    if (ends_with_ci(path, ".mkv"))  return Container::mkv;
    if (ends_with_ci(path, ".ogg"))  return Container::ogg;
    if (ends_with_ci(path, ".wav"))  return Container::wav;
    if (ends_with_ci(path, ".flac")) return Container::flac;
    if (ends_with_ci(path, ".mp3"))  return Container::mp3;
    if (ends_with_ci(path, ".ivf"))  return Container::ivf;
    if (ends_with_ci(path, ".mp4"))  return Container::mp4;
    return fallback;
}

inline const char *container_name(Container c) {
    switch (c) {
        case Container::any:     return "any";
        case Container::mp4:     return "mp4";
        case Container::mov:     return "mov";
        case Container::m4a:     return "m4a";
        case Container::webm:    return "webm";
        case Container::mkv:     return "mkv";
        case Container::ogg:     return "ogg";
        case Container::wav:     return "wav";
        case Container::flac:    return "flac";
        case Container::mp3:     return "mp3";
        case Container::ivf:     return "ivf";
        case Container::unknown: return "unknown";
    }
    return "unknown";
}

inline bool audio_only_container(Container c) {
    return c == Container::m4a || c == Container::wav ||
           c == Container::flac || c == Container::mp3;
}

inline bool mp4_style_container(Container c) {
    return c == Container::mp4 || c == Container::mov || c == Container::m4a ||
           c == Container::unknown;
}

inline bool webm_style_container(Container c) {
    return c == Container::webm || c == Container::mkv;
}

inline bool container_accepts_video(Container c, avb_video_codec codec) {
    switch (c) {
        case Container::any:
            return true;
        case Container::mp4:
        case Container::mov:
            return codec == AVB_VIDEO_CODEC_H264 ||
                   codec == AVB_VIDEO_CODEC_HEVC ||
                   codec == AVB_VIDEO_CODEC_AV1;
        case Container::webm:
        case Container::ivf:
            return codec == AVB_VIDEO_CODEC_VP8 ||
                   codec == AVB_VIDEO_CODEC_VP9 ||
                   codec == AVB_VIDEO_CODEC_AV1;
        case Container::mkv:
            return true;
        default:
            return false;
    }
}

inline bool container_accepts_audio(Container c, avb_audio_codec codec) {
    switch (c) {
        case Container::any:
            return true;
        case Container::mp4:
        case Container::mov:
        case Container::m4a:
            return codec == AVB_AUDIO_CODEC_AAC ||
                   codec == AVB_AUDIO_CODEC_MP3 ||
                   codec == AVB_AUDIO_CODEC_FLAC;
        case Container::webm:
            return codec == AVB_AUDIO_CODEC_OPUS ||
                   codec == AVB_AUDIO_CODEC_VORBIS;
        case Container::mkv:
            return true;
        case Container::ogg:
            return codec == AVB_AUDIO_CODEC_OPUS ||
                   codec == AVB_AUDIO_CODEC_VORBIS ||
                   codec == AVB_AUDIO_CODEC_FLAC;
        case Container::wav:
            return codec == AVB_AUDIO_CODEC_PCM_S16 ||
                   codec == AVB_AUDIO_CODEC_PCM_F32;
        case Container::flac:
            return codec == AVB_AUDIO_CODEC_FLAC;
        case Container::mp3:
            return codec == AVB_AUDIO_CODEC_MP3;
        case Container::ivf:
            return false; // IVF is a video-only elementary stream container.
        case Container::unknown:
            return codec == AVB_AUDIO_CODEC_AAC;
    }
    return false;
}

template <typename T, size_t N>
inline void add_unique(T (&values)[N], int &count, T value) {
    for (int i = 0; i < count; ++i) {
        if (values[i] == value) return;
    }
    if (count < static_cast<int>(N)) values[count++] = value;
}

template <typename Validation>
inline void set_validation_result(Validation &out, avb_result result,
                                  const char *message) {
    out.ok = result == AVB_OK ? 1 : 0;
    out.result = result;
    out.message = message;
}

inline bool valid_pixel_format(avb_pixel_format format) {
    switch (format) {
        case AVB_PIXEL_FORMAT_UNKNOWN:
        case AVB_PIXEL_FORMAT_RGBA8:
        case AVB_PIXEL_FORMAT_BGRA8:
        case AVB_PIXEL_FORMAT_NV12:
        case AVB_PIXEL_FORMAT_I420:
        case AVB_PIXEL_FORMAT_BC1_RGBA:
        case AVB_PIXEL_FORMAT_BC3_RGBA:
        case AVB_PIXEL_FORMAT_BC4_R:
        case AVB_PIXEL_FORMAT_BC5_RG:
        case AVB_PIXEL_FORMAT_BC7_RGBA:
        case AVB_PIXEL_FORMAT_BC3_YCOCG:
        case AVB_PIXEL_FORMAT_BC3_YCOCG_BC4_A:
            return true;
    }
    return false;
}

inline bool valid_video_memory(avb_video_memory_type memory) {
    return memory == AVB_VIDEO_MEMORY_CPU ||
           memory == AVB_VIDEO_MEMORY_BACKEND_NATIVE ||
           memory == AVB_VIDEO_MEMORY_EXTERNAL;
}

inline bool valid_video_external_type(avb_video_external_type external_type) {
    return external_type == AVB_VIDEO_EXTERNAL_NONE ||
           external_type == AVB_VIDEO_EXTERNAL_DMABUF ||
           external_type == AVB_VIDEO_EXTERNAL_D3D11_TEXTURE ||
           external_type == AVB_VIDEO_EXTERNAL_CVPIXEL_BUFFER;
}

inline bool valid_video_memory_external_pair(
    avb_video_memory_type memory,
    avb_video_external_type external_type) {
    if (!valid_video_memory(memory) ||
        !valid_video_external_type(external_type)) {
        return false;
    }
    return memory == AVB_VIDEO_MEMORY_EXTERNAL
        ? external_type != AVB_VIDEO_EXTERNAL_NONE
        : external_type == AVB_VIDEO_EXTERNAL_NONE;
}

inline bool valid_hardware_policy(avb_hardware_policy policy) {
    return policy == AVB_HARDWARE_DISABLED ||
           policy == AVB_HARDWARE_PREFER ||
           policy == AVB_HARDWARE_REQUIRE;
}

inline bool valid_hardware_device(avb_hardware_device device) {
    switch (device) {
        case AVB_HW_DEVICE_AUTO:
        case AVB_HW_DEVICE_VAAPI:
        case AVB_HW_DEVICE_CUDA:
        case AVB_HW_DEVICE_QSV:
        case AVB_HW_DEVICE_D3D11VA:
        case AVB_HW_DEVICE_VIDEOTOOLBOX:
        case AVB_HW_DEVICE_AMF:
            return true;
    }
    return false;
}

inline bool common_video_codec(avb_video_codec codec) {
    return codec == AVB_VIDEO_CODEC_H264 ||
           codec == AVB_VIDEO_CODEC_HEVC ||
           codec == AVB_VIDEO_CODEC_VP8 ||
           codec == AVB_VIDEO_CODEC_VP9 ||
           codec == AVB_VIDEO_CODEC_AV1;
}

inline bool platform_backend(avb_backend backend) {
    return backend == AVB_BACKEND_MEDIAFOUNDATION ||
           backend == AVB_BACKEND_AVFOUNDATION;
}

} // namespace avb::detail

namespace avb::capability {

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
void add_external(
    Capabilities &out,
    avb_video_external_type external_type) {
    add_memory(out, AVB_VIDEO_MEMORY_EXTERNAL);
    detail::add_unique(
        out.video_external_types,
        out.video_external_type_count,
        external_type);
}

template <typename Capabilities>
void add_device(Capabilities &out, avb_hardware_device device) {
    detail::add_unique(
        out.hardware_devices, out.hardware_device_count, device);
}

template <typename Capabilities>
void add_portable_capabilities(
    Capabilities &out,
    detail::Container container) {
    if (!detail::audio_only_container(container))
        add_common_video_codecs(out, container);
    add_common_audio_codecs(out, container);
    add_memory(out, AVB_VIDEO_MEMORY_CPU);
    add_memory(out, AVB_VIDEO_MEMORY_BACKEND_NATIVE);
#if defined(__linux__)
    add_external(out, AVB_VIDEO_EXTERNAL_DMABUF);
#endif
    add_device(out, AVB_HW_DEVICE_AUTO);
    add_device(out, AVB_HW_DEVICE_VAAPI);
}

}  // namespace avb::capability
