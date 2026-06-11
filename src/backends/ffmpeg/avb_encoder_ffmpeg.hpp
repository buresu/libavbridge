#pragma once

#include "../../avb_encoder_backend.hpp"
#include "avb_ffmpeg_loader.hpp"

#include <string>
#include <vector>

// FFmpeg encoder backend (optional, cross-platform). Encodes H.264 video and
// AAC audio into an mp4/mov container. Like the decoder, FFmpeg is loaded at
// runtime via dynamic loading and never linked at build time.
class AvbEncoderFFmpeg : public AvbEncoderBackend {
public:
    AvbEncoderFFmpeg();
    ~AvbEncoderFFmpeg() override;

    avb_result open(const char *path, const avb_encode_options &options) override;
    avb_result write_video(const avb_video_frame &frame, double pts_sec) override;
    avb_result write_audio_f32(const float *src_interleaved, int frames) override;
    avb_result finish() override;
    const char *get_last_error() const override;

private:
    void close_internal();
    // Send one frame (or nullptr to flush) to `enc` and mux every packet it
    // produces into `stream`.
    avb_result encode_and_mux(AVCodecContext *enc, AVStream *stream, AVFrame *frame);
    avb_result encode_audio_frame(int nb_samples); // consumes from m_audio_fifo
    avb_result write_custom_video_packet(avb_encoded_packet &packet);
    avb_result setup_hardware_video_encoder(const avb_encode_options &options,
                                            const AVCodec **out_codec);
    avb_result prepare_software_video_frame(const avb_video_frame &frame, double pts_sec,
                                            AVFrame **out_frame);
    avb_result prepare_hardware_video_frame(const avb_video_frame &frame, double pts_sec,
                                            AVFrame **out_frame);
    avb_result prepare_dmabuf_video_frame(const avb_video_frame &frame, double pts_sec,
                                          AVFrame **out_frame);

    AvbFFmpegFuncs m_ff{};
    bool m_libs_loaded = false;

    AVFormatContext *m_fmt_ctx = nullptr;

    // Video
    AVCodecContext *m_venc        = nullptr;
    AVStream       *m_vstream     = nullptr;
    const avb_video_encoder_plugin *m_custom_video_encoder = nullptr;
    void           *m_custom_video_ctx = nullptr;
    avb_encoded_video_stream m_custom_video_stream{};
    SwsContext     *m_sws         = nullptr;
    AVFrame        *m_vframe      = nullptr; // YUV420P, reused
    AVFrame        *m_hw_vframe   = nullptr;
    AVFrame        *m_drm_import_frame = nullptr;
    AVBufferRef    *m_hw_device_ctx = nullptr;
    AVBufferRef    *m_hw_frames_ctx = nullptr;
    AVPacket       *m_packet      = nullptr;
    AVPixelFormat    m_src_av_fmt   = AV_PIX_FMT_BGRA;
    avb_pixel_format m_input_format = AVB_PIXEL_FORMAT_BGRA8;
    int             m_width       = 0;
    int             m_height      = 0;
    double          m_frame_rate  = 30.0;
    int             m_fps_den     = 30; // encoder time_base = 1 / m_fps_den
    long            m_video_index = 0;  // for derived PTS
    bool            m_has_video   = false;
    bool            m_custom_video = false;
    bool            m_hw_video     = false;
    avb_hardware_device m_hw_device = AVB_HW_DEVICE_AUTO;
    AVPixelFormat   m_hw_pix_fmt   = AV_PIX_FMT_NONE;

    // Audio
    AVCodecContext *m_aenc          = nullptr;
    AVStream       *m_astream       = nullptr;
    SwrContext     *m_swr           = nullptr;
    AVFrame        *m_aframe        = nullptr; // encoder sample_fmt, reused
    int             m_sample_rate   = 0;
    int             m_channels      = 0;
    AVSampleFormat  m_asfmt         = AV_SAMPLE_FMT_FLTP; // encoder input sample fmt
    int             m_frame_size    = 1024;
    int64_t         m_audio_pts     = 0; // in samples
    bool            m_has_audio     = false;
    // Interleaved float FIFO, repacketized into m_frame_size chunks.
    std::vector<float> m_audio_fifo;

    bool m_finished = false;

    std::string m_last_error;

    void set_error(const char *fmt, ...);
    void set_ff_error(const char *prefix, int errnum);
};
