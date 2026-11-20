#pragma once

#include "avb_gstreamer_loader.hpp"
#include "avb_stream_impl.hpp"

#include <string>
#include <vector>

// GStreamer elementary-stream encoder/decoder: appsrc -> codec -> appsink
// pipelines. Hardware encoders are picked by element (V4L2 on Raspberry Pi,
// nvv4l2 on Jetson, VA, NVENC); decoders by decodebin's ranking unless
// hardware is disabled.
class AvbStreamGstPipeline {
public:
    explicit AvbStreamGstPipeline(AvbGstFuncs &gst) : m_gst(gst) {}
    ~AvbStreamGstPipeline() { stop(); }

    // Build and start `description`, which names its elements src/sink (and
    // optionally enc). Returns false with `error` set.
    bool start(const std::string &description, const std::string &src_caps, std::string &error);
    void stop();
    bool running() const { return m_pipeline != nullptr; }

    bool push(const void *data, size_t size, GstClockTime pts, GstClockTime duration,
              bool delta_unit, std::string &error);
    void end_of_stream();
    // Next sample within `timeout` (0 = poll). nullptr when none; `eos` set at the end.
    GstSample *pull(GstClockTime timeout, bool &eos);
    bool bus_error(std::string &error);

    GstElement *encoder() const { return m_enc; }
    AvbGstFuncs &gst() { return m_gst; }

private:
    AvbGstFuncs &m_gst;
    GstElement *m_pipeline = nullptr;
    GstElement *m_src = nullptr;
    GstElement *m_sink = nullptr;
    GstElement *m_enc = nullptr;
};

class AvbStreamEncoderGStreamer : public AvbStreamEncoderImpl {
public:
    AvbStreamEncoderGStreamer();
    ~AvbStreamEncoderGStreamer() override;

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
    AvbGstFuncs m_gst{};
    bool m_loaded = false;
    AvbStreamGstPipeline *m_pipe = nullptr;
    bool m_is_video = false;
    AvbStreamVideoEncodeConfig m_video;
    AvbStreamAudioEncodeConfig m_audio;
    // Live bitrate control: property name and whether it counts kbit/s.
    std::string m_bitrate_property;
    bool m_bitrate_kbps = true;
    int64_t m_audio_samples = 0;
    bool m_flushing = false;
    bool m_eos = false;
    std::vector<unsigned char> m_stage;
    AvbStreamEncoderOutput m_output;
    std::string m_last_error;
};

class AvbStreamDecoderGStreamer : public AvbStreamDecoderImpl {
public:
    AvbStreamDecoderGStreamer();
    ~AvbStreamDecoderGStreamer() override;

    avb_result open(const AvbStreamDecodeConfig &config) override;
    avb_result send(const AvbStreamDecodeInput &input) override;
    avb_result receive_video(avb_video_frame &out) override;
    void release_video(avb_video_frame &) override {}
    avb_result receive_audio(std::vector<float> &out, int &frames, double &pts_sec) override;
    avb_result reset() override;
    void audio_format(int &sample_rate, int &channels) const override;
    const char *get_last_error() const override;

private:
    avb_result start_pipeline();
    GstSample *next_sample(avb_result &result);

    AvbGstFuncs m_gst{};
    bool m_loaded = false;
    AvbStreamGstPipeline *m_pipe = nullptr;
    AvbStreamDecodeConfig m_config;
    std::string m_description;
    std::string m_src_caps;
    bool m_draining = false;
    bool m_eos = false;
    int m_sample_rate = 0;
    int m_channels = 0;
    std::vector<unsigned char> m_video_buf;
    std::string m_last_error;
};
