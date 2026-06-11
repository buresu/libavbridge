#pragma once

#include "../../avb_encoder_impl.hpp"
#include <string>
#include <vector>

// Windows Media Foundation encoder implementation. Wraps an IMFSinkWriter that muxes
// H.264 video and AAC audio into an mp4/mov container. The Sink Writer loads the
// platform H.264/AAC encoder MFTs and automatically inserts the color converter
// (RGB32 -> NV12) the encoder needs, so callers can feed packed BGRA/RGBA (or
// NV12) frames and interleaved float audio directly. Mirrors the FFmpeg and
// AVFoundation encoder implementations.
class AvbEncoderMediaFoundation : public AvbEncoderImpl {
public:
    AvbEncoderMediaFoundation();
    ~AvbEncoderMediaFoundation() override;

    avb_result open(const char *path, const avb_encode_options &options) override;
    avb_result write_video(const avb_video_frame &frame, double pts_sec) override;
    avb_result write_audio_f32(const float *src_interleaved, int frames) override;
    avb_result finish() override;
    const char *get_last_error() const override;

private:
    struct Impl;
    Impl *m_impl = nullptr;
    std::string m_last_error;
};
