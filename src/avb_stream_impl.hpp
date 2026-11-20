#pragma once

#include "avbridge.h"

#include <cstdint>
#include <memory>
#include <vector>

// Backend side of the elementary-stream API (avb_stream_encoder /
// avb_stream_decoder). The shared layer in avb_stream.cpp validates input,
// resolves defaults, tracks H.264/HEVC parameter sets, converts NAL framing,
// waits for keyframes and buffers audio; a backend only drives its codec.

// Video timestamps are ticks of 1/90000 s, audio ones samples.
constexpr int AVB_STREAM_VIDEO_TIMESCALE = 90000;

// Resolved video encode request: codec, frame_rate and bitrate are never
// AUTO/0, CPU input_format is never UNKNOWN, bitstream is never AUTO.
// max_b_frames is -1 for the backend default and 0 whenever low_latency is
// set; gop_size 0 and profile AUTO also leave the backend default.
struct AvbStreamVideoEncodeConfig {
    avb_video_stream_encode_params params{};
};

// sample_rate/channels positive, codec AAC or Opus (or another explicit one).
struct AvbStreamAudioEncodeConfig {
    avb_audio_stream_encode_params params{};
};

struct AvbStreamEncodedPacket {
    std::vector<uint8_t> data;
    int64_t pts = -1;      // stream ticks
    int64_t dts = -1;      // stream ticks, -1 = same as pts
    int64_t duration = -1; // stream ticks, -1 = unknown
    bool keyframe = false;
};

// How a backend's encoder frames its H.264/HEVC output.
struct AvbStreamEncoderOutput {
    bool length_prefixed = false;
    int nal_length_size = 4;
    // Optional codec configuration known at open or after the first packet:
    // avcC/hvcC or Annex-B parameter sets (video), ASC/OpusHead (audio).
    std::vector<uint8_t> extradata;
    int encoder_delay = 0;
    avb_hardware_device hardware_device = AVB_HW_DEVICE_AUTO;
};

class AvbStreamEncoderImpl {
public:
    virtual ~AvbStreamEncoderImpl() = default;

    virtual avb_result open_video(const AvbStreamVideoEncodeConfig &config) = 0;
    virtual avb_result open_audio(const AvbStreamAudioEncodeConfig &config) = 0;
    // `frame` is validated against the configuration; pts_sec is resolved.
    virtual avb_result send_video(const avb_video_frame &frame, double pts_sec,
                                  bool force_keyframe) = 0;
    // Contiguous samples; the backend stamps them from its own running count
    // starting at 0 (the shared layer adds the stream's start offset).
    virtual avb_result send_audio(const float *interleaved, int frames) = 0;
    // AVB_OK, AVB_ERROR_AGAIN, or AVB_ERROR_EOF once flushed and drained.
    virtual avb_result receive(AvbStreamEncodedPacket &out) = 0;
    virtual avb_result set_bitrate(int bitrate) = 0;
    virtual avb_result flush() = 0;
    // May change after the first packets (e.g. extradata learned late).
    virtual const AvbStreamEncoderOutput &output() const = 0;
    virtual const char *get_last_error() const = 0;
};

struct AvbStreamDecodeConfig {
    avb_stream_decode_params params{}; // codec resolved, extradata pointer cleared
    // Video H.264/HEVC: avcC/hvcC built from the known parameter sets (may be
    // empty until the first keyframe). Audio: ASC or OpusHead (may be empty
    // for Opus).
    std::vector<uint8_t> extradata;
};

struct AvbStreamDecodeInput {
    const uint8_t *data = nullptr;
    size_t size = 0;
    double pts_sec = -1.0;
    bool keyframe = false;
    // H.264/HEVC: the parameter sets changed with (or before) this packet;
    // `config` is the avcC/hvcC for the new ones.
    bool config_changed = false;
    const std::vector<uint8_t> *config = nullptr;
};

class AvbStreamDecoderImpl {
public:
    virtual ~AvbStreamDecoderImpl() = default;

    // H.264/HEVC input framing the backend wants: Annex-B with in-band
    // parameter sets before every keyframe, or 4-byte length prefixes with
    // the parameter sets stripped (delivered through `config` instead).
    virtual bool wants_length_prefixed() const { return false; }

    virtual avb_result open(const AvbStreamDecodeConfig &config) = 0;
    // data == nullptr drains.
    virtual avb_result send(const AvbStreamDecodeInput &input) = 0;
    // Frames in the configured video_format/memory. AVB_ERROR_AGAIN / EOF.
    virtual avb_result receive_video(avb_video_frame &out) = 0;
    virtual void release_video(avb_video_frame &frame) = 0;
    // One decoded chunk of interleaved float audio, replacing `out`.
    virtual avb_result receive_audio(std::vector<float> &out, int &frames,
                                     double &pts_sec) = 0;
    virtual avb_result reset() = 0;
    // Decoded audio format, 0/0 until known.
    virtual void audio_format(int &sample_rate, int &channels) const = 0;
    virtual avb_hardware_device hardware_device() const { return AVB_HW_DEVICE_AUTO; }
    virtual const char *get_last_error() const = 0;
};

std::unique_ptr<AvbStreamEncoderImpl> avb_create_stream_encoder_backend(avb_backend backend);
std::unique_ptr<AvbStreamDecoderImpl> avb_create_stream_decoder_backend(avb_backend backend);
