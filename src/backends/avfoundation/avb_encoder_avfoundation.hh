#pragma once

#include "../../avb_encoder_backend.hpp"
#include <string>

class AvbEncoderAVFoundation : public AvbEncoderBackend {
public:
    AvbEncoderAVFoundation();
    ~AvbEncoderAVFoundation() override;

    avb_result open(const char *path, const avb_encode_options &options) override;
    avb_result write_video(const avb_video_frame &frame, double pts_sec) override;
    avb_result write_audio_f32(const float *src_interleaved, int frames) override;
    avb_result finish() override;
    const char *get_last_error() const override;

private:
    struct Impl;
    bool drain(); // append queued samples to whichever inputs are ready

    Impl *m_impl = nullptr;
    std::string m_last_error;
};
