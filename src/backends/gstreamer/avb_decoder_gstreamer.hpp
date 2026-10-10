#pragma once

#include "avb_audio_buffer.hpp"
#include "avb_decoder_impl.hpp"
#include "avb_gstreamer_loader.hpp"

#include <string>
#include <vector>

// GStreamer backend (Linux, opt-in). Built on a single `playbin` whose
// audio-sink and video-sink are small `appsink` bins that convert to the
// canonical output formats (interleaved F32LE audio, packed RGBA/BGRA or planar
// NV12 video). Like the ffmpeg backend, GStreamer is loaded at runtime via
// dlopen and never linked at build time.
//
// playbin picks the video decoder by rank, and current GStreamer ranks the
// hardware ones it trusts (va on Intel/AMD, nvcodec, v4l2) above software. So
// for CPU frames the hardware policy is a matter of what playbin is allowed to
// plug: DISABLED forces software, PREFER takes playbin's choice and falls back
// to software when a hardware decoder was plugged and produced nothing,
// REQUIRE fails unless the choice was hardware. Hardware frames are read back
// by mapping the decoder's own buffer, never by having it copy them out.
class AvbDecoderGStreamer : public AvbDecoderImpl {
public:
    AvbDecoderGStreamer();
    ~AvbDecoderGStreamer() override;

    avb_result open_file(const char *path, const avb_decode_options &options) override;
    avb_result get_media_info(avb_media_info &out_info) override;
    avb_result seek(double seconds) override;
    int read_audio_f32(float *dst_interleaved, int frames) override;
    double audio_next_pts() override;
    bool audio_read_failed() const override { return m_audio_failed; }
    avb_result read_video_frame(avb_video_frame &out_frame) override;
    void release_video_frame(avb_video_frame &frame) override;
    const char *get_last_error() const override;
    const char *get_backend_name() const override;

private:
    void close_internal();
    bool fill_audio_buffer();
    bool check_bus_error();
    avb_result pull_sample(GstElement *sink, GstSample *&sample);
    void discover_codec_names(const char *uri);
    bool validate_track_selection(const avb_decode_options &options);
    avb_result open_custom_file(const char *path, const avb_decode_options &options);
    avb_result open_playbin(const char *path, const avb_decode_options &options,
                            bool software_only);
    bool find_hardware_video_decoder(avb_hardware_device &out_device);
    avb_color_range declared_color_range(GstElement *decoder);
    void fill_color_metadata(const GstCaps *caps, avb_video_frame &frame);
    void release_cpu_video_frame();
    avb_result read_custom_video_frame(avb_video_frame &out_frame);
    avb_result fill_dmabuf_video_frame(GstSample *sample, GstBuffer *buf,
                                       GstCaps *caps, int w, int h,
                                       double pts_sec, avb_video_frame &out_frame);

    AvbGstFuncs m_gst{};
    bool m_libs_loaded = false;

    GstElement *m_pipeline   = nullptr; // playbin
    GstElement *m_audio_sink = nullptr; // appsink (owned ref)
    GstElement *m_video_sink = nullptr; // appsink (owned ref)
    GstSample  *m_video_preroll_sample = nullptr; // metadata preroll or pending first frame
    GstSample  *m_native_video_sample = nullptr; // held until release_video_frame
    // A CPU frame is the decoder's buffer, mapped: the sample and its mapping
    // are held until release_video_frame (or the next read).
    GstSample  *m_cpu_video_sample = nullptr;
    GstMapInfo  m_cpu_video_map{};

    // Effective audio output format (after convert/resample).
    int m_out_sample_rate = 0;
    int m_out_channels    = 0;
    int m_req_sample_rate = 0; // 0 = source
    int m_req_channels    = 0; // 0 = source

    // Audio track selection (playbin "current-audio" is a logical 0-based index).
    int m_audio_track       = 0; // selected logical track
    int m_audio_track_count = 0; // playbin "n-audio"
    int m_video_track       = 0;
    int m_video_track_count = 0;

    int    m_width      = 0;
    int    m_height     = 0;
    double m_frame_rate = 0.0;
    double m_duration   = 0.0;

    avb_pixel_format m_video_format = AVB_PIXEL_FORMAT_BGRA8;
    avb_video_memory_type m_video_memory = AVB_VIDEO_MEMORY_CPU;
    avb_video_external_type m_video_external_type =
        AVB_VIDEO_EXTERNAL_NONE;
    avb_hardware_device m_hw_device = AVB_HW_DEVICE_AUTO;
    // What decodes the CPU frames: AUTO for a software decoder (and for a
    // hardware one of a family this API has no name for).
    avb_hardware_device m_cpu_hw_device = AVB_HW_DEVICE_AUTO;
    // The colour range the stream itself declares, for a hardware decoder
    // whose caps do not carry it. UNKNOWN when the caps are all there is.
    avb_color_range m_stream_color_range = AVB_COLOR_RANGE_UNKNOWN;
    bool m_custom_pipeline = false;
    const avb_video_decoder_plugin *m_custom_video_decoder = nullptr;
    void *m_custom_video_ctx = nullptr;

    // Decoded interleaved-float audio waiting to be consumed (drain loop and
    // head-PTS bookkeeping live in AvbAudioBuffer).
    AvbAudioBuffer     m_audio;

    bool m_audio_eof = false;
    bool m_audio_failed = false;
    bool m_pipeline_failed = false;

    // Target time of the last seek (seconds), or < 0 when none is pending.
    double m_seek_target = -1.0;

    std::string m_last_error;
    std::string m_audio_codec_name;
    std::string m_video_codec_name;

    void set_error(const char *fmt, ...);
};
