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
using avb::detail::common_video_codec;
using avb::detail::mp4_style_container;
using avb::detail::platform_backend;
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

static bool ffmpeg_container_accepts_video(Container c, avb_video_codec codec) {
    return c != Container::any && container_accepts_video(c, codec);
}

static bool ffmpeg_supports_video_codec(avb_video_codec codec) {
    return common_video_codec(codec);
}

static bool gstreamer_supports_video_codec(avb_video_codec codec) {
    return common_video_codec(codec);
}

static bool platform_video_codec(avb_backend backend, avb_video_codec codec) {
    if (backend == AVB_BACKEND_AVFOUNDATION)
        return codec == AVB_VIDEO_CODEC_H264 || codec == AVB_VIDEO_CODEC_HEVC;
    if (backend == AVB_BACKEND_MEDIAFOUNDATION)
        return codec == AVB_VIDEO_CODEC_H264 ||
               codec == AVB_VIDEO_CODEC_HEVC ||
               codec == AVB_VIDEO_CODEC_VP8 ||
               codec == AVB_VIDEO_CODEC_VP9 ||
               codec == AVB_VIDEO_CODEC_AV1;
    return false;
}

static bool platform_container(avb_backend backend, Container c) {
    if (backend == AVB_BACKEND_MEDIAFOUNDATION &&
        (c == Container::ivf || c == Container::wav ||
         c == Container::flac || c == Container::mp3))
        return true;
    return mp4_style_container(c);
}

static bool platform_audio_codec(avb_backend backend, Container c,
                                 avb_audio_codec codec) {
    if (codec == AVB_AUDIO_CODEC_AAC) return mp4_style_container(c);
    if (backend == AVB_BACKEND_MEDIAFOUNDATION)
        return (c == Container::wav &&
               (codec == AVB_AUDIO_CODEC_PCM_S16 ||
                 codec == AVB_AUDIO_CODEC_PCM_F32)) ||
               (c == Container::flac && codec == AVB_AUDIO_CODEC_FLAC) ||
               (c == Container::mp3 && codec == AVB_AUDIO_CODEC_MP3);
    return false;
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

static bool decoder_platform_native(
    avb_backend backend,
    const avb_decode_options &options) {
    if (backend == AVB_BACKEND_MEDIAFOUNDATION) {
        return options.video_memory == AVB_VIDEO_MEMORY_NATIVE &&
               (options.video_format == AVB_PIXEL_FORMAT_UNKNOWN ||
                options.video_format == AVB_PIXEL_FORMAT_NV12) &&
               (options.hardware_device == AVB_HW_DEVICE_AUTO ||
                options.hardware_device == AVB_HW_DEVICE_D3D11VA);
    }
    if (backend == AVB_BACKEND_AVFOUNDATION) {
        return options.video_memory == AVB_VIDEO_MEMORY_NATIVE &&
               (options.hardware_device == AVB_HW_DEVICE_AUTO ||
                options.hardware_device == AVB_HW_DEVICE_VIDEOTOOLBOX);
    }
    return false;
}

} // namespace

extern "C" {

avb_result avb_decoder_validate_options(
    const avb_decode_options *options,
    avb_decoder_validation *out) {
    if (!out) return AVB_ERROR_INVALID_ARGUMENT;

    avb_decode_options defaults{};
    if (!options) {
        defaults = avb_decode_options_default();
        options = &defaults;
    }

    *out = {};
    out->backend = resolve_backend(options->backend);
    out->backend_name = avb_backend_name(out->backend);
    out->video_memory = options->video_memory;
    out->hardware_policy = options->hardware_policy;
    out->hardware_device = options->hardware_device;

    if (!options->enable_audio && !options->enable_video) {
        set_validation_result(
            *out,
            AVB_ERROR_INVALID_ARGUMENT,
            "Decoder requires at least one enabled track.");
        return AVB_OK;
    }
    if (options->audio_stream_index < -1 ||
        options->video_stream_index < -1) {
        set_validation_result(
            *out,
            AVB_ERROR_INVALID_ARGUMENT,
            "Stream indices must be -1 for auto/default or a non-negative index.");
        return AVB_OK;
    }
    if (options->audio_sample_rate < 0 || options->audio_channels < 0) {
        set_validation_result(
            *out,
            AVB_ERROR_INVALID_ARGUMENT,
            "Audio sample_rate/channels must be zero or positive.");
        return AVB_OK;
    }
    if (!valid_pixel_format(options->video_format)) {
        set_validation_result(
            *out,
            AVB_ERROR_INVALID_ARGUMENT,
            "Invalid decoded video pixel format.");
        return AVB_OK;
    }
    if (!valid_video_memory(options->video_memory)) {
        set_validation_result(
            *out,
            AVB_ERROR_INVALID_ARGUMENT,
            "Invalid decoded video memory type.");
        return AVB_OK;
    }
    if (!valid_hardware_policy(options->hardware_policy)) {
        set_validation_result(
            *out, AVB_ERROR_INVALID_ARGUMENT, "Invalid hardware policy.");
        return AVB_OK;
    }
    if (!valid_hardware_device(options->hardware_device)) {
        set_validation_result(
            *out, AVB_ERROR_INVALID_ARGUMENT, "Invalid hardware device.");
        return AVB_OK;
    }
    if (options->video_memory != AVB_VIDEO_MEMORY_CPU &&
        options->hardware_policy == AVB_HARDWARE_DISABLED) {
        set_validation_result(
            *out,
            AVB_ERROR_INVALID_ARGUMENT,
            "Native/DMABUF video output requires hardware_policy PREFER or REQUIRE.");
        return AVB_OK;
    }

    if (!out->backend_name || !avb_backend_is_available(out->backend)) {
        set_validation_result(
            *out,
            AVB_ERROR_BACKEND_NOT_AVAILABLE,
            "Requested decoder backend is not available in this build.");
        return AVB_OK;
    }

    const bool platform_native =
        decoder_platform_native(out->backend, *options);
    if (platform_backend(out->backend) &&
        ((options->video_memory != AVB_VIDEO_MEMORY_CPU &&
          !platform_native) ||
         (options->hardware_policy == AVB_HARDWARE_REQUIRE &&
          !platform_native))) {
        set_validation_result(
            *out,
            AVB_ERROR_OPEN_FAILED,
            "The platform backend cannot produce the requested native video output.");
        return AVB_OK;
    }
    if (out->backend == AVB_BACKEND_GSTREAMER &&
        options->video_memory != AVB_VIDEO_MEMORY_CPU &&
        options->hardware_device != AVB_HW_DEVICE_AUTO &&
        options->hardware_device != AVB_HW_DEVICE_VAAPI) {
        set_validation_result(
            *out,
            AVB_ERROR_INVALID_ARGUMENT,
            "GStreamer native/DMABUF decode currently supports only AUTO/VAAPI devices.");
        return AVB_OK;
    }
#if !defined(__linux__)
    if (options->video_memory == AVB_VIDEO_MEMORY_DMABUF) {
        set_validation_result(
            *out,
            AVB_ERROR_INVALID_ARGUMENT,
            "DMABUF decode output is only supported on Linux builds.");
        return AVB_OK;
    }
#endif

    set_validation_result(
        *out, AVB_OK, "Decode options are statically supported.");
    return AVB_OK;
}

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
            if (!platform_container(out->backend, container)) {
                set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                           "The platform backend does not validate this output container.");
                return AVB_OK;
            }
            if (options->video.enable && !custom_video &&
                !platform_video_codec(out->backend, video_codec)) {
                set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                           "The platform backend supports only H.264/HEVC built-in video encoding.");
                return AVB_OK;
            }
            if (out->backend == AVB_BACKEND_MEDIAFOUNDATION &&
                container == Container::ivf) {
                if (!options->video.enable ||
                    (video_codec != AVB_VIDEO_CODEC_VP8 &&
                     video_codec != AVB_VIDEO_CODEC_VP9 &&
                     video_codec != AVB_VIDEO_CODEC_AV1) ||
                    options->audio.enable) {
                    set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                               "Media Foundation IVF output supports video-only VP8/VP9/AV1.");
                    return AVB_OK;
                }
                break;
            }
            if (options->audio.enable &&
                !platform_audio_codec(out->backend, container, audio_codec)) {
                set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                           "The platform backend does not support this built-in audio codec/container.");
                return AVB_OK;
            }
            {
                const bool mf_native_in =
                    out->backend == AVB_BACKEND_MEDIAFOUNDATION &&
                    options->video.input_memory == AVB_VIDEO_MEMORY_NATIVE &&
                    (options->video.input_format == AVB_PIXEL_FORMAT_UNKNOWN ||
                     options->video.input_format == AVB_PIXEL_FORMAT_NV12) &&
                    (container == Container::ivf ||
                     container == Container::mp4 ||
                     container == Container::mov);
                // AVFoundation appends the caller's IOSurface-backed
                // CVPixelBuffer straight into the AVAssetWriter (zero-copy).
                const bool avf_native_in =
                    out->backend == AVB_BACKEND_AVFOUNDATION &&
                    options->video.input_memory == AVB_VIDEO_MEMORY_NATIVE &&
                    (container == Container::mp4 ||
                     container == Container::mov ||
                     container == Container::unknown);
                if (options->video.enable &&
                    options->video.input_memory != AVB_VIDEO_MEMORY_CPU &&
                    !mf_native_in && !avf_native_in) {
                    set_result(*out, AVB_ERROR_OPEN_FAILED,
                               "The platform backend cannot consume the requested native video input.");
                    return AVB_OK;
                }
            }
            if (out->backend == AVB_BACKEND_MEDIAFOUNDATION &&
                options->video.enable &&
                options->video.input_memory == AVB_VIDEO_MEMORY_NATIVE &&
                container != Container::ivf &&
                !options->video.hardware_context) {
                set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                           "Media Foundation native MP4/MOV input requires an ID3D11Device hardware_context.");
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
