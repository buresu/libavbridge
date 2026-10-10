#include "avbridge.h"
#include "avb_encoder_impl.hpp"

#include <cmath>
#include <memory>
#include <string>
#include <utility>

struct avb_encoder {
    std::unique_ptr<AvbEncoderImpl> impl;
    std::string last_error;
    avb_video_encode_params video{};
    bool opened = false;
    bool finished = false;

    void set_error(const char *message) {
        last_error = message ? message : "";
    }
};

namespace {

// Copy the implementation's last error onto the C handle so it survives even if
// the impl is later reset (e.g. on close).
void capture_error(avb_encoder *enc) {
    const char *err = enc->impl->get_last_error();
    if (err) enc->set_error(err);
}

// Validate CPU planes before any backend or plugin can read them. Native
// handles keep their backend-specific validation.
const char *prepare_video_frame(const avb_encoder &enc, avb_video_frame &frame) {
    if (!enc.video.enable) return "The encoder has no video track.";
    if (frame.width != enc.video.width || frame.height != enc.video.height)
        return "Video frame dimensions do not match encoder options.";
    if (frame.memory_type != enc.video.input_memory ||
        (frame.memory_type == AVB_VIDEO_MEMORY_EXTERNAL &&
         frame.external_type != enc.video.input_external_type))
        return "Video frame memory representation does not match encoder options.";
    if (frame.memory_type != AVB_VIDEO_MEMORY_CPU) return nullptr;

    const auto format = enc.video.input_format == AVB_PIXEL_FORMAT_UNKNOWN
        ? AVB_PIXEL_FORMAT_BGRA8 : enc.video.input_format;
    if (frame.format != format)
        return "Frame pixel format does not match configured input_format.";

    const int64_t width = frame.width;
    int planes = 1;
    int64_t row_bytes[AVB_MAX_PLANES] = {};
    int64_t rows = frame.height;
    switch (format) {
        case AVB_PIXEL_FORMAT_RGBA8:
        case AVB_PIXEL_FORMAT_BGRA8:
            row_bytes[0] = width * 4;
            // Packed-format convenience input uses only data/stride.
            if (frame.plane_count == 0) {
                frame.plane_count = 1;
                frame.plane_data[0] = frame.data;
                frame.plane_stride[0] = frame.stride;
            }
            break;
        case AVB_PIXEL_FORMAT_NV12:
            planes = 2;
            row_bytes[0] = width;
            row_bytes[1] = ((width + 1) / 2) * 2;
            break;
        case AVB_PIXEL_FORMAT_I420:
            planes = 3;
            row_bytes[0] = width;
            row_bytes[1] = row_bytes[2] = (width + 1) / 2;
            break;
        default:
            rows = (rows + 3) / 4;
            row_bytes[0] = ((width + 3) / 4) *
                ((format == AVB_PIXEL_FORMAT_BC1_RGBA ||
                  format == AVB_PIXEL_FORMAT_BC4_R) ? 8 : 16);
            if (format == AVB_PIXEL_FORMAT_BC3_YCOCG_BC4_A) {
                planes = 2;
                row_bytes[1] = ((width + 3) / 4) * 8;
            }
            break;
    }
    if (frame.plane_count != planes)
        return "Video frame has an incorrect plane count.";
    for (int p = 0; p < planes; ++p) {
        if (!frame.plane_data[p] || frame.plane_stride[p] < row_bytes[p])
            return "Video frame has a missing plane or an insufficient positive stride.";
    }
    if (planes == 1 && frame.data_size > 0 &&
        (!frame.data || frame.data == frame.plane_data[0]) &&
        frame.data_size < (rows - 1) * frame.plane_stride[0] + row_bytes[0])
        return "Video frame backing buffer is too small.";
    frame.data = frame.plane_data[0];
    frame.stride = frame.plane_stride[0];
    return nullptr;
}

} // namespace

extern "C" {

avb_encode_options avb_encode_options_default(void) {
    avb_encode_options o{};
    o.backend     = AVB_BACKEND_AUTO;
    o.video.codec = AVB_VIDEO_CODEC_AUTO;
    o.video.input_memory = AVB_VIDEO_MEMORY_CPU;
    o.video.input_external_type = AVB_VIDEO_EXTERNAL_NONE;
    o.video.hardware_policy = AVB_HARDWARE_DISABLED;
    o.video.hardware_device = AVB_HW_DEVICE_AUTO;
    o.audio.codec = AVB_AUDIO_CODEC_AUTO;
    return o;
}

avb_result avb_encoder_open(avb_encoder **out_enc, const char *path,
                            const avb_encode_options *options) {
    if (!out_enc || !path || !options) return AVB_ERROR_INVALID_ARGUMENT;

    auto *enc = new avb_encoder();

    avb_encoder_validation validation{};
    avb_result validation_res = avb_encoder_validate_options(path, options, &validation);
    if (validation_res != AVB_OK) {
        enc->set_error("Invalid encoder validation arguments.");
        *out_enc = enc;
        return validation_res;
    }
    if (!validation.ok) {
        enc->set_error(validation.message);
        *out_enc = enc;
        return validation.result;
    }

    auto impl = avb_create_encoder_backend(options->backend);
    if (!impl) {
        enc->set_error("Requested encoder backend is not available on this platform.");
        *out_enc = enc;
        return AVB_ERROR_BACKEND_NOT_AVAILABLE;
    }
    enc->impl = std::move(impl);

    avb_result res = enc->impl->open(path, *options);
    if (res != AVB_OK) capture_error(enc);
    enc->opened = res == AVB_OK;
    enc->video = options->video;
    *out_enc = enc;
    return res;
}

avb_result avb_encoder_write_video(avb_encoder *enc, const avb_video_frame *frame,
                                   double pts_sec) {
    if (!enc || !frame) return AVB_ERROR_INVALID_ARGUMENT;
    if (!enc->opened || enc->finished) return AVB_ERROR_INVALID_ARGUMENT;
    avb_video_frame input = *frame;
    const char *error = prepare_video_frame(*enc, input);
    if (!error && (!std::isfinite(pts_sec) || !std::isfinite(frame->pts_sec)))
        error = "Video timestamps must be finite.";
    if (error) {
        enc->set_error(error);
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    enc->last_error.clear();
    const avb_result res = enc->impl->write_video(input, pts_sec);
    if (res != AVB_OK) capture_error(enc);
    return res;
}

avb_result avb_encoder_write_audio_f32(avb_encoder *enc, const float *src_interleaved,
                                       int frames) {
    if (!enc || !src_interleaved || frames <= 0) return AVB_ERROR_INVALID_ARGUMENT;
    if (!enc->opened || enc->finished) return AVB_ERROR_INVALID_ARGUMENT;
    enc->last_error.clear();
    const avb_result res = enc->impl->write_audio_f32(src_interleaved, frames);
    if (res != AVB_OK) capture_error(enc);
    return res;
}

avb_result avb_encoder_finish(avb_encoder *enc) {
    if (!enc || !enc->opened || enc->finished) return AVB_ERROR_INVALID_ARGUMENT;
    enc->finished = true;
    enc->last_error.clear();
    const avb_result res = enc->impl->finish();
    if (res != AVB_OK) capture_error(enc);
    return res;
}

const char *avb_encoder_get_last_error(avb_encoder *enc) {
    if (!enc) return nullptr;
    if (!enc->last_error.empty()) return enc->last_error.c_str();
    if (enc->impl) {
        const char *err = enc->impl->get_last_error();
        if (err && err[0] != '\0') return err;
    }
    return nullptr;
}

void avb_encoder_close(avb_encoder *enc) {
    delete enc;
}

} // extern "C"
