#include "avbridge.h"
#include "avb_capability_common.hpp"
#include "avb_video_codec_registry.hpp"

namespace {

using avb::detail::Container;
using avb::detail::audio_only_container;
using avb::detail::container_accepts_audio;
using avb::detail::container_accepts_video;
using avb::detail::container_from_path;
using avb::detail::container_name;
using avb::detail::mp4_style_container;
using avb::detail::resolve_backend;

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

static bool ffmpeg_container_accepts_video(Container c, avb_video_codec codec) {
    return c != Container::any && container_accepts_video(c, codec);
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
    return mp4_style_container(c);
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
    Container container = container_from_path(path, Container::unknown);
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
    if (options->video.enable && audio_only_container(container)) {
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
