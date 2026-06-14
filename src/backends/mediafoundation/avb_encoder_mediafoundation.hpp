#pragma once

#include "../../avb_encoder_impl.hpp"
#include <string>
#include <vector>

// Windows Media Foundation encoder implementation. Wraps an IMFSinkWriter that muxes
// H.264/HEVC video and supported audio into platform containers. The Sink
// Writer loads the platform encoder MFTs and accepts CPU frames or D3D11 NV12
// surfaces configured through avb_video_encode_params::hardware_context.
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
    avb_result open_ivf_video(const char *path, const avb_encode_options &options);
    avb_result process_video_mft_output();
    avb_result drain_video_mft(long long time_hns, long long dur_hns);
    avb_result wait_async_video_input();
    avb_result drain_async_video_mft();
    avb_result drain_audio_mft(long long time_hns, long long dur_hns);
    Impl *m_impl = nullptr;
    std::string m_last_error;
};
