#pragma once

#include "avb_stream_impl.hpp"

#include <memory>
#include <string>

// Apple elementary-stream encoder/decoder: VideoToolbox compression and
// decompression sessions for H.264/HEVC, AudioConverter for AAC (and Opus
// where AudioToolbox provides it). VideoToolbox frames H.264/HEVC with NAL
// length prefixes and keeps the parameter sets in format descriptions.
class AvbStreamEncoderAVFoundation : public AvbStreamEncoderImpl {
public:
    AvbStreamEncoderAVFoundation();
    ~AvbStreamEncoderAVFoundation() override;

    avb_result open_video(const AvbStreamVideoEncodeConfig &config) override;
    avb_result open_audio(const AvbStreamAudioEncodeConfig &config) override;
    avb_result send_video(const avb_video_frame &frame, double pts_sec,
                          bool force_keyframe) override;
    avb_result send_audio(const float *interleaved, int frames) override;
    avb_result receive(AvbStreamEncodedPacket &out) override;
    avb_result set_bitrate(int bitrate) override;
    avb_result flush() override;
    const AvbStreamEncoderOutput &output() const override { return m_output; }
    const char *get_last_error() const override;

    struct Impl;

private:
    avb_result encode_audio(bool final_chunk);
    std::unique_ptr<Impl> m_impl;
    AvbStreamEncoderOutput m_output;
    std::string m_last_error;
};

class AvbStreamDecoderAVFoundation : public AvbStreamDecoderImpl {
public:
    AvbStreamDecoderAVFoundation();
    ~AvbStreamDecoderAVFoundation() override;

    bool wants_length_prefixed() const override { return true; }
    avb_result open(const AvbStreamDecodeConfig &config) override;
    avb_result send(const AvbStreamDecodeInput &input) override;
    avb_result receive_video(avb_video_frame &out) override;
    void release_video(avb_video_frame &frame) override;
    avb_result receive_audio(std::vector<float> &out, int &frames, double &pts_sec) override;
    avb_result reset() override;
    void audio_format(int &sample_rate, int &channels) const override;
    avb_hardware_device hardware_device() const override;
    const char *get_last_error() const override;

    struct Impl;

private:
    avb_result create_session(const std::vector<uint8_t> &config);
    std::unique_ptr<Impl> m_impl;
    std::string m_last_error;
};
