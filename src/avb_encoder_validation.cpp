#include "avbridge.h"
#include "avb_encoder_validation_backend.hpp"
#include "avb_media_rules.hpp"
#include "avb_video_plugins.hpp"

namespace {

using avb::detail::Container;
using avb::detail::audio_only_container;
using avb::detail::container_accepts_audio;
using avb::detail::container_from_path;
using avb::detail::container_name;
using avb::detail::resolve_backend;
using avb::detail::set_validation_result;
using avb::detail::valid_hardware_device;
using avb::detail::valid_hardware_policy;
using avb::detail::valid_pixel_format;
using avb::detail::valid_video_memory;

static void set_result(avb_encoder_validation &out, avb_result result,
                       const char *message) {
    set_validation_result(out, result, message);
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
    if (options->video.enable &&
        !valid_pixel_format(options->video.input_format)) {
        set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                   "Invalid encoded video input pixel format.");
        return AVB_OK;
    }
    if (options->video.enable &&
        !valid_video_memory(options->video.input_memory)) {
        set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                   "Invalid encoded video input memory type.");
        return AVB_OK;
    }
    if (options->video.enable &&
        !valid_hardware_policy(options->video.hardware_policy)) {
        set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                   "Invalid hardware policy.");
        return AVB_OK;
    }
    if (options->video.enable &&
        !valid_hardware_device(options->video.hardware_device)) {
        set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                   "Invalid hardware device.");
        return AVB_OK;
    }
#if !defined(__linux__)
    if (options->video.enable &&
        options->video.input_memory == AVB_VIDEO_MEMORY_DMABUF) {
        set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                   "DMABUF encode input is only supported on Linux builds.");
        return AVB_OK;
    }
#endif

    if (!out->backend_name || !avb_backend_is_available(out->backend)) {
        set_result(*out, AVB_ERROR_BACKEND_NOT_AVAILABLE,
                   "Requested encoder backend is not available in this build.");
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

    avb::validation::EncoderContext context{
        container,
        video_codec,
        audio_codec,
        custom_video,
    };
    if (!avb::validation::validate_encoder_backend(
            *options, context, *out)) {
        return AVB_OK;
    }

    set_result(*out, AVB_OK, "Encode options are statically supported.");
    return AVB_OK;
}

} // extern "C"
