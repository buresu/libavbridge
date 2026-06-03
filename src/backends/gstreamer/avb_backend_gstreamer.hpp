#pragma once

#include "../../avb_backend.hpp"
#include "avb_gstreamer_loader.hpp"

#include <string>
#include <vector>

// GStreamer backend (Linux, opt-in). Built on a single `playbin` whose
// audio-sink and video-sink are small `appsink` bins that convert to the
// canonical output formats (interleaved F32LE audio, packed RGBA/BGRA or planar
// NV12 video). Like the ffmpeg backend, GStreamer is loaded at runtime via
// dlopen and never linked at build time.
class AvbBackendGStreamer : public AvbBackend {
public:
    AvbBackendGStreamer();
    ~AvbBackendGStreamer() override;

    avb_result open_file(const char *path, const avb_open_options &options) override;
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
    void discover_codec_names(const char *uri);

    AvbGstFuncs m_gst{};
    bool m_libs_loaded = false;

    GstElement *m_pipeline   = nullptr; // playbin
    GstElement *m_audio_sink = nullptr; // appsink (owned ref)
    GstElement *m_video_sink = nullptr; // appsink (owned ref)

    // Effective audio output format (after convert/resample).
    int m_out_sample_rate = 0;
    int m_out_channels    = 0;
    int m_req_sample_rate = 0; // 0 = source
    int m_req_channels    = 0; // 0 = source

    int    m_width      = 0;
    int    m_height     = 0;
    double m_frame_rate = 0.0;
    double m_duration   = 0.0;

    avb_pixel_format m_video_format = AVB_PIXEL_FORMAT_BGRA8;

    // Decoded audio waiting to be consumed (interleaved float).
    std::vector<float> m_audio_buf;
    int                m_audio_buf_pos = 0;

    // Backend-owned video frame buffer, reused per read_video_frame call.
    std::vector<unsigned char> m_video_out_buf;

    bool m_audio_eof = false;

    // Target time of the last seek (seconds), or < 0 when none is pending.
    double m_seek_target = -1.0;

    std::string m_last_error;
    std::string m_audio_codec_name;
    std::string m_video_codec_name;

    void set_error(const char *fmt, ...);
};
