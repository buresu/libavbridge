#pragma once

#include "../../avb_encoder_impl.hpp"
#include <string>

class AvbEncoderAVFoundation : public AvbEncoderImpl {
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
    avb_result write_custom_video_packet(avb_encoded_packet &packet, double fallback_pts);

    Impl *m_impl = nullptr;
    std::string m_last_error;
};
