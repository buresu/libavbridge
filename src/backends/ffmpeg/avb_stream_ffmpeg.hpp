#pragma once

#include "avb_ffmpeg_loader.hpp"
#include "avb_stream_impl.hpp"

#include <deque>
#include <string>
#include <vector>

// FFmpeg elementary-stream encoder/decoder: libavcodec's send/receive API
// without libavformat. Hardware encoders that take system-memory frames
// (NVENC, AMF, QSV, VideoToolbox, V4L2 M2M) are tried under PREFER/REQUIRE;
// decoding is software.
class AvbStreamEncoderFFmpeg : public AvbStreamEncoderImpl {
public:
    AvbStreamEncoderFFmpeg();
    ~AvbStreamEncoderFFmpeg() override;

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
    void close_internal();
    void set_ff_error(const char *prefix, int err);
    // Move every packet the encoder has ready into m_queue.
    int pull_packets();
    avb_result encode_audio_chunk(int frames);

    AvbFFmpegFuncs m_ff{};
    bool m_loaded = false;
    AVCodecContext *m_ctx = nullptr;
    AVFrame *m_frame = nullptr;
    AVPacket *m_pkt = nullptr;
    SwsContext *m_sws = nullptr;
    SwrContext *m_swr = nullptr;
    bool m_is_video = false;
    AvbStreamVideoEncodeConfig m_video;
    AvbStreamAudioEncodeConfig m_audio;
    AVPixelFormat m_src_fmt = AV_PIX_FMT_BGRA;
    int m_frame_size = 1024;
    bool m_pad_last = false;
    std::vector<float> m_fifo;
    int64_t m_audio_pts = 0;
    bool m_flushing = false;
    bool m_eof = false;
    std::deque<AvbStreamEncodedPacket> m_queue;
    AvbStreamEncoderOutput m_output;
    std::string m_last_error;
};

class AvbStreamDecoderFFmpeg : public AvbStreamDecoderImpl {
public:
    AvbStreamDecoderFFmpeg();
    ~AvbStreamDecoderFFmpeg() override;

    avb_result open(const AvbStreamDecodeConfig &config) override;
    avb_result send(const AvbStreamDecodeInput &input) override;
    avb_result receive_video(avb_video_frame &out) override;
    void release_video(avb_video_frame &) override {}
    avb_result receive_audio(std::vector<float> &out, int &frames, double &pts_sec) override;
    avb_result reset() override;
    void audio_format(int &sample_rate, int &channels) const override;
    const char *get_last_error() const override;

private:
    void close_internal();
    void clear_frames();
    void set_ff_error(const char *prefix, int err);
    // Next decoded frame (queued or fresh) into m_frame. AVERROR codes.
    int next_frame();

    AvbFFmpegFuncs m_ff{};
    bool m_loaded = false;
    AVCodecContext *m_ctx = nullptr;
    AVFrame *m_frame = nullptr;
    AVPacket *m_pkt = nullptr;
    SwsContext *m_sws = nullptr;
    SwrContext *m_swr = nullptr;
    std::deque<AVFrame *> m_pending;
    AvbStreamDecodeConfig m_config;
    int m_timescale = 90000;
    int m_sws_w = 0, m_sws_h = 0;
    AVPixelFormat m_sws_fmt = AV_PIX_FMT_NONE;
    AVColorSpace m_sws_space = AVCOL_SPC_UNSPECIFIED;
    AVColorRange m_sws_range = AVCOL_RANGE_UNSPECIFIED;
    int m_swr_rate = 0, m_swr_channels = 0, m_swr_fmt = -1;
    int m_sample_rate = 0, m_channels = 0;
    std::vector<unsigned char> m_video_buf;
    std::string m_last_error;
};
