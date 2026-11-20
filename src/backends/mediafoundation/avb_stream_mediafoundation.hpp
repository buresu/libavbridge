#pragma once

#include "avb_stream_impl.hpp"

#include <memory>
#include <string>

// Media Foundation elementary-stream encoder/decoder. Drives encoder and
// decoder MFTs directly (no Sink Writer / Source Reader), synchronous or
// asynchronous (hardware), and hands their samples over as packets/frames.
// Video: H.264/HEVC (and VP8/VP9/AV1 where the system has the MFTs). Audio:
// AAC only -- Windows ships no Opus encoder/decoder MFT.
class AvbStreamEncoderMediaFoundation : public AvbStreamEncoderImpl {
public:
    AvbStreamEncoderMediaFoundation();
    ~AvbStreamEncoderMediaFoundation() override;

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

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    AvbStreamEncoderOutput m_output;
    std::string m_last_error;
};

class AvbStreamDecoderMediaFoundation : public AvbStreamDecoderImpl {
public:
    AvbStreamDecoderMediaFoundation();
    ~AvbStreamDecoderMediaFoundation() override;

    avb_result open(const AvbStreamDecodeConfig &config) override;
    avb_result send(const AvbStreamDecodeInput &input) override;
    avb_result receive_video(avb_video_frame &out) override;
    void release_video(avb_video_frame &frame) override;
    avb_result receive_audio(std::vector<float> &out, int &frames, double &pts_sec) override;
    avb_result reset() override;
    void audio_format(int &sample_rate, int &channels) const override;
    avb_hardware_device hardware_device() const override;
    const char *get_last_error() const override;

private:
    struct Impl;
    avb_result configure_video(const std::vector<uint8_t> &config);
    std::unique_ptr<Impl> m_impl;
    std::string m_last_error;
};
