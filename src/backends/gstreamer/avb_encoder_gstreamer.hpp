#pragma once

#include "../../avb_encoder_backend.hpp"
#include "avb_gstreamer_loader.hpp"

#include <string>
#include <vector>

// GStreamer encoder backend. Builds an appsrc -> encoder -> muxer -> filesink
// pipeline (H.264 via x264enc, AAC via avenc_aac, into mp4/mov) and pushes
// caller-supplied frames/audio into the two appsrc elements. Like the decoder,
// GStreamer is loaded at runtime via dlopen and never linked at build time.
class AvbEncoderGStreamer : public AvbEncoderBackend {
public:
    AvbEncoderGStreamer();
    ~AvbEncoderGStreamer() override;

    avb_result open(const char *path, const avb_encode_options &options) override;
    avb_result write_video(const avb_video_frame &frame, double pts_sec) override;
    avb_result write_audio_f32(const float *src_interleaved, int frames) override;
    avb_result finish() override;
    const char *get_last_error() const override;

private:
    void close_internal();
    avb_result check_bus_error(); // poll the bus, surface any ERROR message

    AvbGstFuncs m_gst{};
    bool m_libs_loaded = false;

    GstElement *m_pipeline = nullptr;
    GstElement *m_vsrc     = nullptr; // appsrc (owned ref)
    GstElement *m_asrc     = nullptr; // appsrc (owned ref)

    bool   m_has_video = false;
    bool   m_has_audio = false;
    int    m_width = 0, m_height = 0;
    double m_frame_rate = 30.0;
    int    m_fps_n = 30; // framerate numerator (denominator fixed at 1)
    avb_pixel_format m_input_format = AVB_PIXEL_FORMAT_BGRA8;

    int     m_sample_rate = 0;
    int     m_channels    = 0;
    long    m_video_index = 0; // for derived PTS
    int64_t m_audio_samples = 0;

    bool m_finished = false;

    std::string m_last_error;
    std::vector<unsigned char> m_stage; // video repack scratch
};
