#include "avb_encoder_validation_backend.hpp"

namespace avb::validation {
namespace {

using detail::Container;
using detail::common_video_codec;
using detail::container_accepts_video;
using detail::mp4_style_container;
using detail::set_validation_result;

void set_result(
    avb_encoder_validation &out,
    avb_result result,
    const char *message) {
    set_validation_result(out, result, message);
}

bool portable_container_accepts_video(
    Container container,
    avb_video_codec codec) {
    return container != Container::any &&
           container_accepts_video(container, codec);
}

bool platform_video_codec(avb_backend backend, avb_video_codec codec) {
    if (backend == AVB_BACKEND_AVFOUNDATION)
        return codec == AVB_VIDEO_CODEC_H264 ||
               codec == AVB_VIDEO_CODEC_HEVC;
    if (backend == AVB_BACKEND_MEDIAFOUNDATION)
        return codec == AVB_VIDEO_CODEC_H264 ||
               codec == AVB_VIDEO_CODEC_HEVC ||
               codec == AVB_VIDEO_CODEC_VP8 ||
               codec == AVB_VIDEO_CODEC_VP9 ||
               codec == AVB_VIDEO_CODEC_AV1;
    return false;
}

bool platform_container(avb_backend backend, Container container) {
    if (backend == AVB_BACKEND_MEDIAFOUNDATION &&
        (container == Container::ivf ||
         container == Container::wav ||
         container == Container::flac ||
         container == Container::mp3)) {
        return true;
    }
    return mp4_style_container(container);
}

bool platform_audio_codec(
    avb_backend backend,
    Container container,
    avb_audio_codec codec) {
    if (codec == AVB_AUDIO_CODEC_AAC)
        return mp4_style_container(container);
    if (backend == AVB_BACKEND_MEDIAFOUNDATION) {
        return (container == Container::wav &&
                (codec == AVB_AUDIO_CODEC_PCM_S16 ||
                 codec == AVB_AUDIO_CODEC_PCM_F32)) ||
               (container == Container::flac &&
                codec == AVB_AUDIO_CODEC_FLAC) ||
               (container == Container::mp3 &&
                codec == AVB_AUDIO_CODEC_MP3);
    }
    return false;
}

bool platform_native_input(
    avb_backend backend,
    Container container,
    const avb_video_encode_params &video) {
    if (backend == AVB_BACKEND_MEDIAFOUNDATION) {
        return video.input_memory == AVB_VIDEO_MEMORY_EXTERNAL &&
               video.input_external_type ==
                   AVB_VIDEO_EXTERNAL_D3D11_TEXTURE &&
               (video.input_format == AVB_PIXEL_FORMAT_UNKNOWN ||
                video.input_format == AVB_PIXEL_FORMAT_NV12) &&
               (container == Container::ivf ||
                container == Container::mp4 ||
                container == Container::mov);
    }
    if (backend == AVB_BACKEND_AVFOUNDATION) {
        return video.input_memory == AVB_VIDEO_MEMORY_EXTERNAL &&
               video.input_external_type ==
                   AVB_VIDEO_EXTERNAL_CVPIXEL_BUFFER &&
               (container == Container::mp4 ||
                container == Container::mov ||
                container == Container::unknown);
    }
    return false;
}

bool validate_portable_backend(
    const avb_encode_options &options,
    const EncoderContext &context,
    avb_encoder_validation &out,
    bool gstreamer) {
    if (options.video.enable &&
        options.video.input_memory == AVB_VIDEO_MEMORY_EXTERNAL &&
        options.video.input_external_type != AVB_VIDEO_EXTERNAL_DMABUF) {
        set_result(
            out,
            AVB_ERROR_OPEN_FAILED,
            "The selected backend cannot import the requested external video type.");
        return false;
    }
    if (options.video.enable && !context.custom_video &&
        !common_video_codec(context.video_codec)) {
        set_result(
            out,
            AVB_ERROR_INVALID_ARGUMENT,
            gstreamer
                ? "GStreamer does not support the requested built-in video codec."
                : "FFmpeg does not support the requested built-in video codec.");
        return false;
    }
    if (options.video.enable && !context.custom_video &&
        !portable_container_accepts_video(
            context.container, context.video_codec)) {
        set_result(
            out,
            AVB_ERROR_INVALID_ARGUMENT,
            "The output container does not support the requested video codec.");
        return false;
    }
    if (gstreamer && options.video.enable &&
        options.video.hardware_policy == AVB_HARDWARE_REQUIRE &&
        context.video_codec != AVB_VIDEO_CODEC_H264 &&
        context.video_codec != AVB_VIDEO_CODEC_HEVC &&
        context.video_codec != AVB_VIDEO_CODEC_VP9) {
        set_result(
            out,
            AVB_ERROR_INVALID_ARGUMENT,
            "GStreamer hardware encoding is not statically supported for this codec.");
        return false;
    }
    return true;
}

bool validate_platform_backend(
    const avb_encode_options &options,
    const EncoderContext &context,
    avb_encoder_validation &out) {
    if (!platform_container(out.backend, context.container)) {
        set_result(
            out,
            AVB_ERROR_INVALID_ARGUMENT,
            "The platform backend does not validate this output container.");
        return false;
    }
    if (options.video.enable && !context.custom_video &&
        !platform_video_codec(out.backend, context.video_codec)) {
        set_result(
            out,
            AVB_ERROR_INVALID_ARGUMENT,
            "The platform backend supports only H.264/HEVC built-in video encoding.");
        return false;
    }
    if (out.backend == AVB_BACKEND_MEDIAFOUNDATION &&
        context.container == Container::ivf) {
        if (!options.video.enable ||
            (context.video_codec != AVB_VIDEO_CODEC_VP8 &&
             context.video_codec != AVB_VIDEO_CODEC_VP9 &&
             context.video_codec != AVB_VIDEO_CODEC_AV1) ||
            options.audio.enable) {
            set_result(
                out,
                AVB_ERROR_INVALID_ARGUMENT,
                "Media Foundation IVF output supports video-only VP8/VP9/AV1.");
            return false;
        }
        return true;
    }
    if (options.audio.enable &&
        !platform_audio_codec(
            out.backend, context.container, context.audio_codec)) {
        set_result(
            out,
            AVB_ERROR_INVALID_ARGUMENT,
            "The platform backend does not support this built-in audio codec/container.");
        return false;
    }
    if (options.video.enable &&
        options.video.input_memory != AVB_VIDEO_MEMORY_CPU &&
        !platform_native_input(
            out.backend, context.container, options.video)) {
        set_result(
            out,
            AVB_ERROR_OPEN_FAILED,
            "The platform backend cannot consume the requested video memory representation.");
        return false;
    }
    if (out.backend == AVB_BACKEND_MEDIAFOUNDATION &&
        options.video.enable &&
        options.video.input_memory == AVB_VIDEO_MEMORY_EXTERNAL &&
        options.video.input_external_type ==
            AVB_VIDEO_EXTERNAL_D3D11_TEXTURE &&
        context.container != Container::ivf &&
        !options.video.hardware_context) {
        set_result(
            out,
            AVB_ERROR_INVALID_ARGUMENT,
            "Media Foundation D3D11 texture input requires an ID3D11Device hardware_context.");
        return false;
    }
    return true;
}

}  // namespace

bool validate_encoder_backend(
    const avb_encode_options &options,
    const EncoderContext &context,
    avb_encoder_validation &out) {
    switch (out.backend) {
        case AVB_BACKEND_FFMPEG:
            return validate_portable_backend(
                options, context, out, false);
        case AVB_BACKEND_GSTREAMER:
            return validate_portable_backend(
                options, context, out, true);
        case AVB_BACKEND_AVFOUNDATION:
        case AVB_BACKEND_MEDIAFOUNDATION:
            return validate_platform_backend(options, context, out);
        default:
            set_result(
                out,
                AVB_ERROR_BACKEND_NOT_AVAILABLE,
                "Requested encoder backend is not available.");
            return false;
    }
}

}  // namespace avb::validation
