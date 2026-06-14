#include "avbridge.h"
#include "avb_encoder_impl.hpp"

#include <memory>
#include <string>
#include <utility>

struct avb_encoder {
    std::unique_ptr<AvbEncoderImpl> impl;
    std::string last_error;

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
    *out_enc = enc;
    return res;
}

avb_result avb_encoder_write_video(avb_encoder *enc, const avb_video_frame *frame,
                                   double pts_sec) {
    if (!enc || !frame) return AVB_ERROR_INVALID_ARGUMENT;
    if (!enc->impl) return AVB_ERROR_INVALID_ARGUMENT;
    return enc->impl->write_video(*frame, pts_sec);
}

avb_result avb_encoder_write_audio_f32(avb_encoder *enc, const float *src_interleaved,
                                       int frames) {
    if (!enc || !src_interleaved || frames <= 0) return AVB_ERROR_INVALID_ARGUMENT;
    if (!enc->impl) return AVB_ERROR_INVALID_ARGUMENT;
    return enc->impl->write_audio_f32(src_interleaved, frames);
}

avb_result avb_encoder_finish(avb_encoder *enc) {
    if (!enc || !enc->impl) return AVB_ERROR_INVALID_ARGUMENT;
    return enc->impl->finish();
}

const char *avb_encoder_get_last_error(avb_encoder *enc) {
    if (!enc) return nullptr;
    if (enc->impl) {
        const char *err = enc->impl->get_last_error();
        if (err && err[0] != '\0') return err;
    }
    return enc->last_error.empty() ? nullptr : enc->last_error.c_str();
}

void avb_encoder_close(avb_encoder *enc) {
    delete enc;
}

} // extern "C"
