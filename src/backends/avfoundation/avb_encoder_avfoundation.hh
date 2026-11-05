#pragma once

#include "avb_encoder_impl.hpp"
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
    // The public entry points run these behind an Objective-C exception guard:
    // AVAssetWriter raises NSExceptions for settings it rejects, and one that
    // escapes through the C API takes the whole process down.
    avb_result open_impl(const char *path, const avb_encode_options &options);
    avb_result write_video_impl(const avb_video_frame &frame, double pts_sec);
    avb_result write_audio_impl(const float *src_interleaved, int frames);
    avb_result finish_impl();
    bool drain(); // append queued samples to whichever inputs are ready
    avb_result write_custom_video_packet(avb_encoded_packet &packet, double fallback_pts);

    Impl *m_impl = nullptr;
    std::string m_last_error;
};
