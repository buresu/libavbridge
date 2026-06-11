#pragma once

#include "avbridge.h"

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
        case Container::unknown: return "unknown";
    }
    return "unknown";
}

inline avb_backend resolve_backend(avb_backend backend) {
    if (backend != AVB_BACKEND_AUTO) return backend;
#if defined(_WIN32)
    return AVB_BACKEND_MEDIAFOUNDATION;
#elif defined(__APPLE__)
    return AVB_BACKEND_AVFOUNDATION;
#elif defined(__linux__)
#  if defined(AVB_ENABLE_GSTREAMER)
    return AVB_BACKEND_GSTREAMER;
#  elif defined(AVB_ENABLE_FFMPEG)
    return AVB_BACKEND_FFMPEG;
#  else
    return AVB_BACKEND_AUTO;
#  endif
#else
    return AVB_BACKEND_AUTO;
#endif
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
        case Container::unknown:
            return codec == AVB_AUDIO_CODEC_AAC;
    }
    return false;
}

} // namespace avb::detail
