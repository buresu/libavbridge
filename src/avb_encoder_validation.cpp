#include "avbridge.h"
#include "avb_video_codec_registry.hpp"

#include <cctype>
#include <cstring>

namespace {

enum class Container {
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

static bool ends_with_ci(const char *path, const char *suffix) {
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
    if (ends_with_ci(path, ".mov"))  return Container::mov;
    if (ends_with_ci(path, ".m4a"))  return Container::m4a;
    if (ends_with_ci(path, ".webm")) return Container::webm;
    if (ends_with_ci(path, ".mkv"))  return Container::mkv;
    if (ends_with_ci(path, ".ogg"))  return Container::ogg;
    if (ends_with_ci(path, ".wav"))  return Container::wav;
    if (ends_with_ci(path, ".flac")) return Container::flac;
    if (ends_with_ci(path, ".mp3"))  return Container::mp3;
    if (ends_with_ci(path, ".mp4"))  return Container::mp4;
    return Container::unknown;
}

static const char *container_name(Container c) {
    switch (c) {
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

static avb_video_codec resolve_video_codec(avb_video_codec codec) {
    return codec == AVB_VIDEO_CODEC_AUTO ? AVB_VIDEO_CODEC_H264 : codec;
}

static avb_audio_codec resolve_audio_codec(avb_audio_codec codec, Container container) {
    if (codec != AVB_AUDIO_CODEC_AUTO) return codec;
    switch (container) {
        case Container::webm:
        case Container::ogg:
            return AVB_AUDIO_CODEC_OPUS;
        case Container::flac:
            return AVB_AUDIO_CODEC_FLAC;
        case Container::mp3:
            return AVB_AUDIO_CODEC_MP3;
        case Container::wav:
            return AVB_AUDIO_CODEC_PCM_S16;
        default:
            return AVB_AUDIO_CODEC_AAC;
    }
}

static void set_result(avb_encoder_validation &out, avb_result result, const char *message) {
    out.ok = result == AVB_OK ? 1 : 0;
    out.result = result;
    out.message = message;
}

static bool is_audio_only_container(Container c) {
    return c == Container::m4a || c == Container::wav ||
           c == Container::flac || c == Container::mp3;
}

static bool ffmpeg_container_accepts_video(Container c, avb_video_codec codec) {
    switch (c) {
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

static bool ffmpeg_supports_video_codec(avb_video_codec codec) {
    return codec == AVB_VIDEO_CODEC_H264 ||
           codec == AVB_VIDEO_CODEC_HEVC ||
           codec == AVB_VIDEO_CODEC_VP8 ||
           codec == AVB_VIDEO_CODEC_VP9 ||
           codec == AVB_VIDEO_CODEC_AV1;
}

static bool gstreamer_supports_video_codec(avb_video_codec codec) {
    return codec == AVB_VIDEO_CODEC_H264 ||
           codec == AVB_VIDEO_CODEC_HEVC ||
           codec == AVB_VIDEO_CODEC_VP8 ||
           codec == AVB_VIDEO_CODEC_VP9 ||
           codec == AVB_VIDEO_CODEC_AV1;
}

static bool platform_video_codec(avb_backend backend, avb_video_codec codec) {
    if (backend == AVB_BACKEND_AVFOUNDATION)
        return codec == AVB_VIDEO_CODEC_H264 || codec == AVB_VIDEO_CODEC_HEVC;
    if (backend == AVB_BACKEND_MEDIAFOUNDATION)
        return codec == AVB_VIDEO_CODEC_H264 || codec == AVB_VIDEO_CODEC_HEVC;
    return false;
}

static bool platform_container(Container c) {
    return c == Container::mp4 || c == Container::mov || c == Container::m4a ||
           c == Container::unknown;
}

static bool has_custom_video_encoder(const avb_encode_options &options) {
    if (!options.video.enable) return false;
    avb_video_encode_info info{};
    info.width = options.video.width;
    info.height = options.video.height;
    info.frame_rate = options.video.frame_rate > 0 ? options.video.frame_rate : 30.0;
    info.input_format = options.video.input_format;
    info.input_memory = options.video.input_memory;
    info.codec = options.video.codec;
    info.bitrate = options.video.bitrate;
    return avb_find_video_encoder_plugin(info) != nullptr;
}

static bool is_compressed_video_format(avb_pixel_format format) {
    return format == AVB_PIXEL_FORMAT_BC1_RGBA ||
           format == AVB_PIXEL_FORMAT_BC3_RGBA ||
           format == AVB_PIXEL_FORMAT_BC4_R ||
           format == AVB_PIXEL_FORMAT_BC5_RG ||
           format == AVB_PIXEL_FORMAT_BC7_RGBA;
}

} // namespace

extern "C" {

avb_result avb_encoder_validate_options(const char *path,
                                        const avb_encode_options *options,
                                        avb_encoder_validation *out) {
    if (!path || !options || !out) return AVB_ERROR_INVALID_ARGUMENT;

    *out = {};
    Container container = container_from_path(path);
    out->container_name = container_name(container);
    out->backend = resolve_backend(options->backend);
    out->backend_name = avb_backend_name(out->backend);
    out->video_codec = options->video.enable
        ? resolve_video_codec(options->video.codec)
        : AVB_VIDEO_CODEC_AUTO;
    out->audio_codec = options->audio.enable
        ? resolve_audio_codec(options->audio.codec, container)
        : AVB_AUDIO_CODEC_AUTO;
    out->video_codec_name = avb_video_codec_name(out->video_codec);
    out->audio_codec_name = avb_audio_codec_name(out->audio_codec);

    if (!options->video.enable && !options->audio.enable) {
        set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                   "Encoder requires at least one enabled track.");
        return AVB_OK;
    }
    if (!out->backend_name || !avb_backend_is_available(out->backend)) {
        set_result(*out, AVB_ERROR_BACKEND_NOT_AVAILABLE,
                   "Requested encoder backend is not available in this build.");
        return AVB_OK;
    }
    if (options->video.enable && is_audio_only_container(container)) {
        set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                   "The output container is audio-only and cannot mux video.");
        return AVB_OK;
    }
    if (options->video.enable &&
        (options->video.width <= 0 || options->video.height <= 0)) {
        set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                   "Video width/height must be positive.");
        return AVB_OK;
    }
    if (options->audio.enable &&
        (options->audio.sample_rate <= 0 || options->audio.channels <= 0)) {
        set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                   "Audio sample_rate/channels must be positive.");
        return AVB_OK;
    }

    bool custom_video = has_custom_video_encoder(*options);
    avb_video_codec video_codec = out->video_codec;
    avb_audio_codec audio_codec = out->audio_codec;

    if (options->video.enable && is_compressed_video_format(options->video.input_format) &&
        !custom_video) {
        set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                   "Compressed video input requires a registered custom video encoder.");
        return AVB_OK;
    }
    if (options->video.enable && video_codec == AVB_VIDEO_CODEC_HAP && !custom_video) {
        set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                   "HAP encoding requires a registered custom video encoder.");
        return AVB_OK;
    }
    if (options->audio.enable && !container_accepts_audio(container, audio_codec)) {
        set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                   "The output container does not support the requested audio codec.");
        return AVB_OK;
    }

    switch (out->backend) {
        case AVB_BACKEND_FFMPEG:
            if (options->video.enable && !custom_video &&
                !ffmpeg_supports_video_codec(video_codec)) {
                set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                           "FFmpeg does not support the requested built-in video codec.");
                return AVB_OK;
            }
            if (options->video.enable && !custom_video &&
                !ffmpeg_container_accepts_video(container, video_codec)) {
                set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                           "The output container does not support the requested video codec.");
                return AVB_OK;
            }
            break;

        case AVB_BACKEND_GSTREAMER:
            if (options->video.enable && !custom_video &&
                !gstreamer_supports_video_codec(video_codec)) {
                set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                           "GStreamer does not support the requested built-in video codec.");
                return AVB_OK;
            }
            if (options->video.enable && !custom_video &&
                !ffmpeg_container_accepts_video(container, video_codec)) {
                set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                           "The output container does not support the requested video codec.");
                return AVB_OK;
            }
            if (options->video.enable &&
                options->video.hardware_policy == AVB_HARDWARE_REQUIRE &&
                video_codec != AVB_VIDEO_CODEC_H264 &&
                video_codec != AVB_VIDEO_CODEC_HEVC &&
                video_codec != AVB_VIDEO_CODEC_VP9) {
                set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                           "GStreamer hardware encoding is not statically supported for this codec.");
                return AVB_OK;
            }
            break;

        case AVB_BACKEND_AVFOUNDATION:
        case AVB_BACKEND_MEDIAFOUNDATION:
            if (!platform_container(container)) {
                set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                           "The platform backend only validates MP4/MOV/M4A-style outputs.");
                return AVB_OK;
            }
            if (options->video.enable && !custom_video &&
                !platform_video_codec(out->backend, video_codec)) {
                set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                           "The platform backend supports only H.264/HEVC built-in video encoding.");
                return AVB_OK;
            }
            if (options->audio.enable && audio_codec != AVB_AUDIO_CODEC_AAC) {
                set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                           "The platform backend supports only AAC built-in audio encoding.");
                return AVB_OK;
            }
            if (options->video.enable &&
                (options->video.input_memory != AVB_VIDEO_MEMORY_CPU ||
                 options->video.hardware_policy == AVB_HARDWARE_REQUIRE)) {
                set_result(*out, AVB_ERROR_OPEN_FAILED,
                           "The platform backend does not implement native hardware video input yet.");
                return AVB_OK;
            }
            break;

        default:
            set_result(*out, AVB_ERROR_BACKEND_NOT_AVAILABLE,
                       "Requested encoder backend is not available.");
            return AVB_OK;
    }

    set_result(*out, AVB_OK, "Encode options are statically supported.");
    return AVB_OK;
}

} // extern "C"
