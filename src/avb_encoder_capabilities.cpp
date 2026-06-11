#include "avbridge.h"

#include <cctype>
#include <cstring>

namespace {

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
};

static bool ends_with_ci(const char *path, const char *suffix) {
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

static Container container_from_path(const char *path) {
    if (!path || path[0] == '\0') return Container::any;
    if (ends_with_ci(path, ".mov"))  return Container::mov;
    if (ends_with_ci(path, ".m4a"))  return Container::m4a;
    if (ends_with_ci(path, ".webm")) return Container::webm;
    if (ends_with_ci(path, ".mkv"))  return Container::mkv;
    if (ends_with_ci(path, ".ogg"))  return Container::ogg;
    if (ends_with_ci(path, ".wav"))  return Container::wav;
    if (ends_with_ci(path, ".flac")) return Container::flac;
    if (ends_with_ci(path, ".mp3"))  return Container::mp3;
    if (ends_with_ci(path, ".mp4"))  return Container::mp4;
    return Container::any;
}

static const char *container_name(Container c) {
    switch (c) {
        case Container::any:   return "any";
        case Container::mp4:   return "mp4";
        case Container::mov:   return "mov";
        case Container::m4a:   return "m4a";
        case Container::webm:  return "webm";
        case Container::mkv:   return "mkv";
        case Container::ogg:   return "ogg";
        case Container::wav:   return "wav";
        case Container::flac:  return "flac";
        case Container::mp3:   return "mp3";
    }
    return "any";
}

static avb_backend resolve_backend(avb_backend backend) {
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

static bool audio_only_container(Container c) {
    return c == Container::m4a || c == Container::wav ||
           c == Container::flac || c == Container::mp3;
}

static bool container_accepts_video(Container c, avb_video_codec codec) {
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

static bool container_accepts_audio(Container c, avb_audio_codec codec) {
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
    }
    return false;
}

static void add_video_codec(avb_encoder_capabilities &out, avb_video_codec codec,
                            Container container) {
    if (!container_accepts_video(container, codec)) return;
    if (out.video_codec_count >= AVB_MAX_CODEC_CAPS) return;
    out.video_codecs[out.video_codec_count++] = codec;
}

static void add_audio_codec(avb_encoder_capabilities &out, avb_audio_codec codec,
                            Container container) {
    if (!container_accepts_audio(container, codec)) return;
    if (out.audio_codec_count >= AVB_MAX_CODEC_CAPS) return;
    out.audio_codecs[out.audio_codec_count++] = codec;
}

static void add_memory(avb_encoder_capabilities &out, avb_video_memory_type memory) {
    if (out.video_memory_count >= AVB_MAX_VIDEO_MEMORY_CAPS) return;
    out.video_memory[out.video_memory_count++] = memory;
}

static void add_device(avb_encoder_capabilities &out, avb_hardware_device device) {
    if (out.hardware_device_count >= AVB_MAX_HARDWARE_DEVICE_CAPS) return;
    out.hardware_devices[out.hardware_device_count++] = device;
}

static void add_common_software_video(avb_encoder_capabilities &out, Container c) {
    add_video_codec(out, AVB_VIDEO_CODEC_H264, c);
    add_video_codec(out, AVB_VIDEO_CODEC_HEVC, c);
    add_video_codec(out, AVB_VIDEO_CODEC_VP8, c);
    add_video_codec(out, AVB_VIDEO_CODEC_VP9, c);
    add_video_codec(out, AVB_VIDEO_CODEC_AV1, c);
}

static void add_common_audio(avb_encoder_capabilities &out, Container c) {
    add_audio_codec(out, AVB_AUDIO_CODEC_AAC, c);
    add_audio_codec(out, AVB_AUDIO_CODEC_OPUS, c);
    add_audio_codec(out, AVB_AUDIO_CODEC_MP3, c);
    add_audio_codec(out, AVB_AUDIO_CODEC_FLAC, c);
    add_audio_codec(out, AVB_AUDIO_CODEC_VORBIS, c);
    add_audio_codec(out, AVB_AUDIO_CODEC_PCM_S16, c);
    add_audio_codec(out, AVB_AUDIO_CODEC_PCM_F32, c);
}

static void fill_ffmpeg(avb_encoder_capabilities &out, Container c) {
    if (!audio_only_container(c)) add_common_software_video(out, c);
    add_common_audio(out, c);

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
    add_device(out, AVB_HW_DEVICE_AMF);
}

static void fill_gstreamer(avb_encoder_capabilities &out, Container c) {
    if (!audio_only_container(c)) add_common_software_video(out, c);
    add_common_audio(out, c);

    add_memory(out, AVB_VIDEO_MEMORY_CPU);
    add_memory(out, AVB_VIDEO_MEMORY_NATIVE);
#if defined(__linux__)
    add_memory(out, AVB_VIDEO_MEMORY_DMABUF);
#endif

    add_device(out, AVB_HW_DEVICE_AUTO);
    add_device(out, AVB_HW_DEVICE_VAAPI);
}

static void fill_platform(avb_encoder_capabilities &out, Container c) {
    if (!audio_only_container(c)) {
        add_video_codec(out, AVB_VIDEO_CODEC_H264, c);
        add_video_codec(out, AVB_VIDEO_CODEC_HEVC, c);
    }
    add_audio_codec(out, AVB_AUDIO_CODEC_AAC, c);
    add_memory(out, AVB_VIDEO_MEMORY_CPU);
    add_device(out, AVB_HW_DEVICE_AUTO);
}

} // namespace

extern "C" {

avb_result avb_encoder_query_capabilities(avb_backend backend, const char *path,
                                          avb_encoder_capabilities *out) {
    if (!out) return AVB_ERROR_INVALID_ARGUMENT;

    *out = {};
    Container container = container_from_path(path);
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
            out->message = "Requested encoder backend is not available.";
            return AVB_OK;
    }

    out->can_encode_video = out->video_codec_count > 0 ? 1 : 0;
    out->can_encode_audio = out->audio_codec_count > 0 ? 1 : 0;
    out->message = "Encoder capabilities are statically available.";
    return AVB_OK;
}

} // extern "C"
