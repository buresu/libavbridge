#pragma once

#include "../../avb_backend.hpp"
#include "avb_ffmpeg_loader.hpp"

#include <deque>
#include <string>
#include <vector>

class AvbBackendFFmpeg : public AvbBackend {
public:
    AvbBackendFFmpeg();
    ~AvbBackendFFmpeg() override;

    avb_result open_file(const char *path, const avb_decode_options &options) override;
    avb_result get_media_info(avb_media_info &out_info) override;
    avb_result seek(double seconds) override;
    int read_audio_f32(float *dst_interleaved, int frames) override;
    avb_result read_video_frame(avb_video_frame &out_frame) override;
    void release_video_frame(avb_video_frame &frame) override;
    const char *get_last_error() const override;
    const char *get_backend_name() const override;

private:
    void close_internal();
    bool fill_audio_buffer();

    // Demux until a packet for `stream_idx` is available. Packets belonging to
    // the *other* enabled stream are queued (not discarded) so that reading one
    // stream to EOF does not consume the other stream's packets — both share a
    // single AVFormatContext. Returns an owned packet (free with av_packet_free)
    // or nullptr at end of file.
    AVPacket *demux_next(int stream_idx);
    void clear_packet_queues();

    AvbFFmpegFuncs m_ff{};
    bool m_libs_loaded = false;

    AVFormatContext *m_fmt_ctx         = nullptr;
    AVCodecContext  *m_audio_codec_ctx = nullptr;
    AVCodecContext  *m_video_codec_ctx = nullptr;
    AVPacket        *m_packet          = nullptr;
    AVFrame         *m_audio_frame     = nullptr;
    AVFrame         *m_video_frame     = nullptr; // raw decoded video frame
    SwrContext      *m_swr             = nullptr;
    SwsContext      *m_sws             = nullptr; // pixel format conversion

    int m_audio_stream_idx = -1;
    int m_video_stream_idx = -1;

    // Effective audio output format (after optional resample/remix).
    int m_out_sample_rate = 0;
    int m_out_channels    = 0;

    // Cached swscale input dimensions/format to detect changes
    int              m_sws_src_w      = 0;
    int              m_sws_src_h      = 0;
    AVPixelFormat    m_sws_src_fmt    = AV_PIX_FMT_NONE;

    // Requested output pixel format and its libav equivalent.
    avb_pixel_format m_video_format   = AVB_PIXEL_FORMAT_BGRA8;
    AVPixelFormat    m_dst_av_fmt     = AV_PIX_FMT_BGRA;

    // Demuxed-but-not-yet-decoded packets, separated per stream. Filled lazily
    // by demux_next() when a packet for the other stream is encountered.
    std::deque<AVPacket *>     m_audio_pkts;
    std::deque<AVPacket *>     m_video_pkts;

    // Decoded audio samples waiting to be consumed
    std::vector<float>         m_audio_buf;
    int                        m_audio_buf_pos = 0;

    // BGRA output buffer for video frames (owned by backend)
    std::vector<unsigned char> m_video_out_buf;

    bool m_eof = false;

    // Target time of the last seek (seconds), or < 0 when no seek is pending.
    // av_seek_frame lands on the keyframe at or before the request; decoded
    // video frames with pts before this target are dropped so the first frame
    // returned after a seek starts at ~target, matching the AVFoundation
    // backend's timeRange-trimmed behaviour.
    double m_seek_target = -1.0;

    std::string m_last_error;
    std::string m_audio_codec_name;
    std::string m_video_codec_name;

    avb_media_info m_media_info{};

    void set_error(const char *fmt, ...);
    void set_ff_error(const char *prefix, int errnum);
};
