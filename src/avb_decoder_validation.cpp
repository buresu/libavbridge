#include "avbridge.h"
#include "avb_capability_common.hpp"

namespace {

using avb::detail::platform_backend;
using avb::detail::resolve_backend;
using avb::detail::set_validation_result;
using avb::detail::valid_hardware_device;
using avb::detail::valid_hardware_policy;
using avb::detail::valid_pixel_format;
using avb::detail::valid_video_memory;

} // namespace

extern "C" {

avb_result avb_decoder_validate_options(const avb_decode_options *options,
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
        set_validation_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                              "Decoder requires at least one enabled track.");
        return AVB_OK;
    }
    if (!out->backend_name || !avb_backend_is_available(out->backend)) {
        set_validation_result(*out, AVB_ERROR_BACKEND_NOT_AVAILABLE,
                              "Requested decoder backend is not available in this build.");
        return AVB_OK;
    }
    if (options->audio_stream_index < -1 || options->video_stream_index < -1) {
        set_validation_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                              "Stream indices must be -1 for auto/default or a non-negative index.");
        return AVB_OK;
    }
    if (options->audio_sample_rate < 0 || options->audio_channels < 0) {
        set_validation_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                              "Audio sample_rate/channels must be zero or positive.");
        return AVB_OK;
    }
    if (!valid_pixel_format(options->video_format)) {
        set_validation_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                              "Invalid decoded video pixel format.");
        return AVB_OK;
    }
    if (!valid_video_memory(options->video_memory)) {
        set_validation_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                              "Invalid decoded video memory type.");
        return AVB_OK;
    }
    if (!valid_hardware_policy(options->hardware_policy)) {
        set_validation_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                              "Invalid hardware policy.");
        return AVB_OK;
    }
    if (!valid_hardware_device(options->hardware_device)) {
        set_validation_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                              "Invalid hardware device.");
        return AVB_OK;
    }
    if (options->video_memory != AVB_VIDEO_MEMORY_CPU &&
        options->hardware_policy == AVB_HARDWARE_DISABLED) {
        set_validation_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                              "Native/DMABUF video output requires hardware_policy PREFER or REQUIRE.");
        return AVB_OK;
    }
    const bool mediafoundation_native =
        out->backend == AVB_BACKEND_MEDIAFOUNDATION &&
        options->video_memory == AVB_VIDEO_MEMORY_NATIVE &&
        (options->video_format == AVB_PIXEL_FORMAT_UNKNOWN ||
         options->video_format == AVB_PIXEL_FORMAT_NV12) &&
        (options->hardware_device == AVB_HW_DEVICE_AUTO ||
         options->hardware_device == AVB_HW_DEVICE_D3D11VA);
    // AVFoundation hands back the decoder's IOSurface-backed CVPixelBuffer for
    // NATIVE output via VideoToolbox; the format is the decoder's own (NV12 by
    // default) so it is not constrained here.
    const bool avfoundation_native =
        out->backend == AVB_BACKEND_AVFOUNDATION &&
        options->video_memory == AVB_VIDEO_MEMORY_NATIVE &&
        (options->hardware_device == AVB_HW_DEVICE_AUTO ||
         options->hardware_device == AVB_HW_DEVICE_VIDEOTOOLBOX);
    const bool platform_native = mediafoundation_native || avfoundation_native;
    if (platform_backend(out->backend) &&
        ((options->video_memory != AVB_VIDEO_MEMORY_CPU &&
          !platform_native) ||
         (options->hardware_policy == AVB_HARDWARE_REQUIRE &&
          !platform_native))) {
        set_validation_result(*out, AVB_ERROR_OPEN_FAILED,
                              "The platform backend cannot produce the requested native video output.");
        return AVB_OK;
    }
    if (out->backend == AVB_BACKEND_GSTREAMER &&
        options->video_memory != AVB_VIDEO_MEMORY_CPU &&
        options->hardware_device != AVB_HW_DEVICE_AUTO &&
        options->hardware_device != AVB_HW_DEVICE_VAAPI) {
        set_validation_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                              "GStreamer native/DMABUF decode currently supports only AUTO/VAAPI devices.");
        return AVB_OK;
    }
#if !defined(__linux__)
    if (options->video_memory == AVB_VIDEO_MEMORY_DMABUF) {
        set_validation_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                              "DMABUF decode output is only supported on Linux builds.");
        return AVB_OK;
    }
#endif

    set_validation_result(*out, AVB_OK, "Decode options are statically supported.");
    return AVB_OK;
}

} // extern "C"
