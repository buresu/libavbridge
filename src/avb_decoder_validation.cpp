#include "avbridge.h"
#include "avb_media_rules.hpp"

namespace {

using avb::detail::platform_backend;
using avb::detail::resolve_backend;
using avb::detail::set_validation_result;
using avb::detail::valid_hardware_device;
using avb::detail::valid_hardware_policy;
using avb::detail::valid_pixel_format;
using avb::detail::valid_video_memory_external_pair;

bool platform_video_output(
    avb_backend backend,
    const avb_decode_options &options) {
    if (backend == AVB_BACKEND_MEDIAFOUNDATION) {
        return options.video_memory == AVB_VIDEO_MEMORY_EXTERNAL &&
               options.video_external_type ==
                   AVB_VIDEO_EXTERNAL_D3D11_TEXTURE &&
               (options.video_format == AVB_PIXEL_FORMAT_UNKNOWN ||
                options.video_format == AVB_PIXEL_FORMAT_NV12) &&
               (options.hardware_device == AVB_HW_DEVICE_AUTO ||
                options.hardware_device == AVB_HW_DEVICE_D3D11VA);
    }
    if (backend == AVB_BACKEND_AVFOUNDATION) {
        return options.video_memory == AVB_VIDEO_MEMORY_EXTERNAL &&
               options.video_external_type ==
                   AVB_VIDEO_EXTERNAL_CVPIXEL_BUFFER &&
               (options.hardware_device == AVB_HW_DEVICE_AUTO ||
                options.hardware_device == AVB_HW_DEVICE_VIDEOTOOLBOX);
    }
    return false;
}

}  // namespace

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
    out->video_external_type = options->video_external_type;
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
    if (!valid_video_memory_external_pair(
            options->video_memory, options->video_external_type)) {
        set_validation_result(
            *out,
            AVB_ERROR_INVALID_ARGUMENT,
            "Invalid decoded video memory/external type combination.");
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
            "Backend-native/external video output requires hardware_policy PREFER or REQUIRE.");
        return AVB_OK;
    }
    if (!out->backend_name || !avb_backend_is_available(out->backend)) {
        set_validation_result(
            *out,
            AVB_ERROR_BACKEND_NOT_AVAILABLE,
            "Requested decoder backend is not available in this build.");
        return AVB_OK;
    }

    const bool platform_output = platform_video_output(out->backend, *options);
    if (platform_backend(out->backend) &&
        ((options->video_memory != AVB_VIDEO_MEMORY_CPU && !platform_output) ||
         (options->hardware_policy == AVB_HARDWARE_REQUIRE &&
          !platform_output))) {
        set_validation_result(
            *out,
            AVB_ERROR_OPEN_FAILED,
            "The platform backend cannot produce the requested video memory representation.");
        return AVB_OK;
    }
    if (out->backend == AVB_BACKEND_GSTREAMER &&
        options->video_memory != AVB_VIDEO_MEMORY_CPU &&
        options->hardware_device != AVB_HW_DEVICE_AUTO &&
        options->hardware_device != AVB_HW_DEVICE_VAAPI) {
        set_validation_result(
            *out,
            AVB_ERROR_INVALID_ARGUMENT,
            "GStreamer backend-native/external decode supports only AUTO/VAAPI devices.");
        return AVB_OK;
    }
    if ((out->backend == AVB_BACKEND_FFMPEG ||
         out->backend == AVB_BACKEND_GSTREAMER) &&
        options->video_memory == AVB_VIDEO_MEMORY_EXTERNAL &&
        options->video_external_type != AVB_VIDEO_EXTERNAL_DMABUF) {
        set_validation_result(
            *out,
            AVB_ERROR_OPEN_FAILED,
            "The selected backend cannot export the requested external video type.");
        return AVB_OK;
    }
#if !defined(__linux__)
    if (options->video_memory == AVB_VIDEO_MEMORY_EXTERNAL &&
        options->video_external_type == AVB_VIDEO_EXTERNAL_DMABUF) {
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

}  // extern "C"
