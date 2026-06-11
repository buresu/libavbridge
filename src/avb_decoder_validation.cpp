#include "avbridge.h"

namespace {

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

static void set_result(avb_decoder_validation &out, avb_result result, const char *message) {
    out.ok = result == AVB_OK ? 1 : 0;
    out.result = result;
    out.message = message;
}

static bool valid_pixel_format(avb_pixel_format format) {
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
            return true;
    }
    return false;
}

static bool valid_video_memory(avb_video_memory_type memory) {
    switch (memory) {
        case AVB_VIDEO_MEMORY_CPU:
        case AVB_VIDEO_MEMORY_NATIVE:
        case AVB_VIDEO_MEMORY_DMABUF:
            return true;
    }
    return false;
}

static bool valid_hardware_policy(avb_hardware_policy policy) {
    switch (policy) {
        case AVB_HARDWARE_DISABLED:
        case AVB_HARDWARE_PREFER:
        case AVB_HARDWARE_REQUIRE:
            return true;
    }
    return false;
}

static bool valid_hardware_device(avb_hardware_device device) {
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

static bool platform_backend(avb_backend backend) {
    return backend == AVB_BACKEND_MEDIAFOUNDATION ||
           backend == AVB_BACKEND_AVFOUNDATION;
}

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
        set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                   "Decoder requires at least one enabled track.");
        return AVB_OK;
    }
    if (!out->backend_name || !avb_backend_is_available(out->backend)) {
        set_result(*out, AVB_ERROR_BACKEND_NOT_AVAILABLE,
                   "Requested decoder backend is not available in this build.");
        return AVB_OK;
    }
    if (options->audio_stream_index < -1 || options->video_stream_index < -1) {
        set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                   "Stream indices must be -1 for auto/default or a non-negative index.");
        return AVB_OK;
    }
    if (options->audio_sample_rate < 0 || options->audio_channels < 0) {
        set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                   "Audio sample_rate/channels must be zero or positive.");
        return AVB_OK;
    }
    if (!valid_pixel_format(options->video_format)) {
        set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                   "Invalid decoded video pixel format.");
        return AVB_OK;
    }
    if (!valid_video_memory(options->video_memory)) {
        set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                   "Invalid decoded video memory type.");
        return AVB_OK;
    }
    if (!valid_hardware_policy(options->hardware_policy)) {
        set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                   "Invalid hardware policy.");
        return AVB_OK;
    }
    if (!valid_hardware_device(options->hardware_device)) {
        set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                   "Invalid hardware device.");
        return AVB_OK;
    }
    if (options->video_memory != AVB_VIDEO_MEMORY_CPU &&
        options->hardware_policy == AVB_HARDWARE_DISABLED) {
        set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                   "Native/DMABUF video output requires hardware_policy PREFER or REQUIRE.");
        return AVB_OK;
    }
    if (platform_backend(out->backend) &&
        (options->video_memory != AVB_VIDEO_MEMORY_CPU ||
         options->hardware_policy == AVB_HARDWARE_REQUIRE)) {
        set_result(*out, AVB_ERROR_OPEN_FAILED,
                   "The platform backend does not implement native hardware video output yet.");
        return AVB_OK;
    }
    if (out->backend == AVB_BACKEND_GSTREAMER &&
        options->video_memory != AVB_VIDEO_MEMORY_CPU &&
        options->hardware_device != AVB_HW_DEVICE_AUTO &&
        options->hardware_device != AVB_HW_DEVICE_VAAPI) {
        set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                   "GStreamer native/DMABUF decode currently supports only AUTO/VAAPI devices.");
        return AVB_OK;
    }
#if !defined(__linux__)
    if (options->video_memory == AVB_VIDEO_MEMORY_DMABUF) {
        set_result(*out, AVB_ERROR_INVALID_ARGUMENT,
                   "DMABUF decode output is only supported on Linux builds.");
        return AVB_OK;
    }
#endif

    set_result(*out, AVB_OK, "Decode options are statically supported.");
    return AVB_OK;
}

} // extern "C"
