#pragma once

#include "avbridge.h"

#include <memory>

class AvbEncoderImpl {
public:
    virtual ~AvbEncoderImpl() = default;

    virtual avb_result open(const char *path, const avb_encode_options &options) = 0;
    virtual avb_result write_video(const avb_video_frame &frame, double pts_sec) = 0;
    virtual avb_result write_audio_f32(const float *src_interleaved, int frames) = 0;
    virtual avb_result finish() = 0;
    virtual const char *get_last_error() const = 0;
};

std::unique_ptr<AvbEncoderImpl> avb_create_encoder_backend(avb_backend backend);
