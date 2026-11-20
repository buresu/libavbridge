#include "avb_stream_gstreamer.hpp"
#include "avb_plane_layout.hpp"
#include "avb_pixel_convert.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <initializer_list>
#include <cstring>

namespace {

std::string strf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
std::string strf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

const char *raw_video_format(avb_pixel_format format) {
    switch (format) {
        case AVB_PIXEL_FORMAT_RGBA8: return "RGBA";
        case AVB_PIXEL_FORMAT_NV12:  return "NV12";
        case AVB_PIXEL_FORMAT_I420:  return "I420";
        default:                     return "BGRA";
    }
}

bool element_exists(AvbGstFuncs &gst, const char *name) {
    GstElement *probe = gst.gst_element_factory_make(name, nullptr);
    if (!probe) return false;
    gst.gst_object_unref(probe);
    return true;
}

struct VideoEncoderChoice {
    const char *probe;
    std::string chain;              // pre-elements + "<encoder> name=enc ..."
    std::string bitrate_property;   // empty: no live bitrate control
    bool kbps = true;
    bool profile_caps = false;      // accepts a profile through downstream caps
    avb_hardware_device device = AVB_HW_DEVICE_AUTO;
};

std::vector<VideoEncoderChoice> video_encoder_choices(const avb_video_stream_encode_params &p) {
    const avb_video_encode_params &v = p.video;
    const int kbps = std::max(1, v.bitrate / 1000);
    const int bps = v.bitrate;
    const int gop = p.gop_size;
    const int bframes = p.max_b_frames;
    const bool ll = p.low_latency != 0;
    const bool h265 = v.codec == AVB_VIDEO_CODEC_HEVC;
    std::vector<VideoEncoderChoice> hw, sw;

    if (v.codec == AVB_VIDEO_CODEC_H264 || h265) {
        const char *n = h265 ? "265" : "264";
        // Jetson
        {
            VideoEncoderChoice c;
            c.probe = h265 ? "nvv4l2h265enc" : "nvv4l2h264enc";
            c.chain = strf("nvvidconv ! video/x-raw(memory:NVMM),format=NV12 ! %s name=enc "
                             "bitrate=%d insert-sps-pps=true", c.probe, bps);
            if (gop > 0) c.chain += strf(" iframeinterval=%d idrinterval=%d", gop, gop);
            if (ll) c.chain += " maxperf-enable=true";
            c.bitrate_property = "bitrate";
            c.kbps = false;
            c.device = AVB_HW_DEVICE_V4L2;
            hw.push_back(c);
        }
        // Raspberry Pi (stateful V4L2 M2M)
        {
            VideoEncoderChoice c;
            c.probe = h265 ? "v4l2h265enc" : "v4l2h264enc";
            std::string controls = strf("controls,video_bitrate=%d", bps);
            if (gop > 0 && !h265) controls += strf(",h264_i_frame_period=%d", gop);
            if (!h265) controls += ",repeat_sequence_header=1";
            c.chain = strf("%s name=enc extra-controls=\"%s\"", c.probe, controls.c_str());
            if (!h265) {
                const bool high_rate = (double)v.width * v.height * v.frame_rate > 1920.0 * 1080 * 30;
                c.chain += strf(" ! video/x-h264,level=(string)%s", high_rate ? "4.2" : "4");
            }
            c.device = AVB_HW_DEVICE_V4L2;
            hw.push_back(c);
        }
        // VA-API
        {
            VideoEncoderChoice c;
            c.probe = h265 ? "vah265enc" : "vah264enc";
            c.chain = strf("vapostproc ! %s name=enc bitrate=%d", c.probe, kbps);
            if (gop > 0) c.chain += strf(" key-int-max=%d", gop);
            if (bframes >= 0) c.chain += strf(" b-frames=%d", bframes);
            if (p.rate_control == AVB_RATE_CONTROL_CBR) c.chain += " rate-control=cbr";
            c.bitrate_property = "bitrate";
            c.profile_caps = true;
            c.device = AVB_HW_DEVICE_VAAPI;
            hw.push_back(c);
        }
        // NVENC (desktop)
        {
            VideoEncoderChoice c;
            c.probe = h265 ? "nvh265enc" : "nvh264enc";
            c.chain = strf("%s name=enc bitrate=%d", c.probe, kbps);
            if (gop > 0) c.chain += strf(" gop-size=%d", gop);
            if (bframes >= 0) c.chain += strf(" bframes=%d", bframes);
            if (ll) c.chain += " zerolatency=true";
            c.bitrate_property = "bitrate";
            c.device = AVB_HW_DEVICE_CUDA;
            hw.push_back(c);
        }
        VideoEncoderChoice c;
        c.probe = h265 ? "x265enc" : "x264enc";
        c.chain = strf("x%senc name=enc speed-preset=veryfast bitrate=%d", n, kbps);
        if (gop > 0) c.chain += strf(" key-int-max=%d", gop);
        if (!h265) {
            c.chain += " byte-stream=true";
            if (bframes >= 0) c.chain += strf(" bframes=%d", bframes);
        } else if (bframes >= 0) {
            c.chain += strf(" option-string=\"bframes=%d\"", bframes);
        }
        if (ll) c.chain += " tune=zerolatency";
        c.bitrate_property = "bitrate";
        c.profile_caps = true;
        sw.push_back(c);
    } else if (v.codec == AVB_VIDEO_CODEC_VP8 || v.codec == AVB_VIDEO_CODEC_VP9) {
        const bool vp9 = v.codec == AVB_VIDEO_CODEC_VP9;
        if (vp9) {
            VideoEncoderChoice c;
            c.probe = "vavp9lpenc";
            c.chain = strf("vapostproc ! vavp9lpenc name=enc bitrate=%d", kbps);
            if (gop > 0) c.chain += strf(" key-int-max=%d", gop);
            c.bitrate_property = "bitrate";
            c.device = AVB_HW_DEVICE_VAAPI;
            hw.push_back(c);
        }
        VideoEncoderChoice c;
        c.probe = vp9 ? "vp9enc" : "vp8enc";
        c.chain = strf("%s name=enc deadline=1 cpu-used=8 target-bitrate=%d", c.probe, bps);
        if (gop > 0) c.chain += strf(" keyframe-max-dist=%d", gop);
        if (ll) c.chain += " lag-in-frames=0";
        if (p.rate_control == AVB_RATE_CONTROL_CBR) c.chain += " end-usage=cbr";
        c.bitrate_property = "target-bitrate";
        c.kbps = false;
        sw.push_back(c);
    } else {
        VideoEncoderChoice svt;
        svt.probe = "svtav1enc";
        svt.chain = strf("svtav1enc name=enc preset=10 target-bitrate=%d", kbps);
        if (gop > 0) svt.chain += strf(" intra-period-length=%d", gop);
        svt.bitrate_property = "target-bitrate";
        sw.push_back(svt);
        VideoEncoderChoice aom;
        aom.probe = "av1enc";
        aom.chain = strf("av1enc name=enc cpu-used=8 usage-profile=realtime target-bitrate=%d", kbps);
        if (gop > 0) aom.chain += strf(" keyframe-max-dist=%d", gop);
        aom.bitrate_property = "target-bitrate";
        sw.push_back(aom);
    }

    std::vector<VideoEncoderChoice> out;
    if (v.hardware_policy != AVB_HARDWARE_DISABLED) {
        for (const auto &c : hw) {
            const avb_hardware_device want = v.hardware_device;
            if (want == AVB_HW_DEVICE_AUTO || want == c.device ||
                (want == AVB_HW_DEVICE_CUDA && c.device == AVB_HW_DEVICE_V4L2 &&
                 std::strncmp(c.probe, "nvv4l2", 6) == 0))
                out.push_back(c);
        }
    }
    if (v.hardware_policy != AVB_HARDWARE_REQUIRE) out.insert(out.end(), sw.begin(), sw.end());
    return out;
}

const char *h264_profile_caps(avb_video_profile profile) {
    switch (profile) {
        case AVB_VIDEO_PROFILE_H264_CONSTRAINED_BASELINE: return "constrained-baseline";
        case AVB_VIDEO_PROFILE_H264_BASELINE: return "baseline";
        case AVB_VIDEO_PROFILE_H264_MAIN: return "main";
        case AVB_VIDEO_PROFILE_H264_HIGH: return "high";
        default: return nullptr;
    }
}

std::string hex(const std::vector<uint8_t> &bytes) {
    std::string out;
    static const char digits[] = "0123456789abcdef";
    for (uint8_t b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 15]);
    }
    return out;
}

int64_t clock_to_ticks(AvbGstFuncs &gst, GstClockTime time, int timescale) {
    if (!GST_CLOCK_TIME_IS_VALID(time)) return -1;
    return (int64_t)gst.gst_util_uint64_scale_round(time, (guint64)timescale, GST_SECOND);
}

} // namespace

/* =============================================================== pipeline */

bool AvbStreamGstPipeline::start(const std::string &description, const std::string &src_caps,
                                 std::string &error) {
    stop();
    GError *err = nullptr;
    m_pipeline = m_gst.gst_parse_launch(description.c_str(), &err);
    if (err) {
        error = std::string("gst_parse_launch failed: ") + (err->message ? err->message : "unknown");
        m_gst.g_clear_error(&err);
        stop();
        return false;
    }
    if (!m_pipeline) {
        error = "gst_parse_launch failed.";
        return false;
    }
    m_src = m_gst.gst_bin_get_by_name((GstBin *)m_pipeline, "src");
    m_sink = m_gst.gst_bin_get_by_name((GstBin *)m_pipeline, "sink");
    m_enc = m_gst.gst_bin_get_by_name((GstBin *)m_pipeline, "enc");
    if (!m_src || !m_sink) {
        error = "The stream pipeline lacks its appsrc/appsink.";
        stop();
        return false;
    }
    GstCaps *caps = m_gst.gst_caps_from_string(src_caps.c_str());
    if (!caps) {
        error = "Invalid appsrc caps: " + src_caps;
        stop();
        return false;
    }
    m_gst.g_object_set(m_src, "caps", caps, nullptr);
    m_gst.gst_mini_object_unref((GstMiniObject *)caps);
    if (m_gst.gst_element_set_state(m_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        if (!bus_error(error)) error = "The stream pipeline failed to start.";
        stop();
        return false;
    }
    return true;
}

void AvbStreamGstPipeline::stop() {
    if (m_pipeline) m_gst.gst_element_set_state(m_pipeline, GST_STATE_NULL);
    if (m_src) { m_gst.gst_object_unref(m_src); m_src = nullptr; }
    if (m_sink) { m_gst.gst_object_unref(m_sink); m_sink = nullptr; }
    if (m_enc) { m_gst.gst_object_unref(m_enc); m_enc = nullptr; }
    if (m_pipeline) { m_gst.gst_object_unref(m_pipeline); m_pipeline = nullptr; }
}

bool AvbStreamGstPipeline::push(const void *data, size_t size, GstClockTime pts,
                                GstClockTime duration, bool delta_unit, std::string &error) {
    GstBuffer *buf = m_gst.gst_buffer_new_allocate(nullptr, size, nullptr);
    if (!buf) {
        error = "gst_buffer_new_allocate failed.";
        return false;
    }
    m_gst.gst_buffer_fill(buf, 0, data, size);
    GST_BUFFER_PTS(buf) = pts;
    GST_BUFFER_DTS(buf) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION(buf) = duration;
    if (delta_unit) GST_BUFFER_FLAG_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT);
    if (m_gst.gst_app_src_push_buffer((GstAppSrc *)m_src, buf) != GST_FLOW_OK) {
        if (!bus_error(error)) error = "appsrc refused the buffer.";
        return false;
    }
    return true;
}

void AvbStreamGstPipeline::end_of_stream() {
    if (m_src) m_gst.gst_app_src_end_of_stream((GstAppSrc *)m_src);
}

GstSample *AvbStreamGstPipeline::pull(GstClockTime timeout, bool &eos) {
    eos = false;
    if (!m_sink) return nullptr;
    GstSample *sample = m_gst.gst_app_sink_try_pull_sample((GstAppSink *)m_sink, timeout);
    if (!sample) eos = m_gst.gst_app_sink_is_eos((GstAppSink *)m_sink) != FALSE;
    return sample;
}

bool AvbStreamGstPipeline::bus_error(std::string &error) {
    if (!m_pipeline) return false;
    GstBus *bus = m_gst.gst_element_get_bus(m_pipeline);
    if (!bus) return false;
    GstMessage *msg = m_gst.gst_bus_timed_pop_filtered(bus, 0, GST_MESSAGE_ERROR);
    bool found = false;
    if (msg) {
        GError *err = nullptr;
        gchar *debug = nullptr;
        m_gst.gst_message_parse_error(msg, &err, &debug);
        error = err && err->message ? err->message : "GStreamer pipeline error.";
        if (err) m_gst.g_error_free(err);
        if (debug) m_gst.g_free(debug);
        m_gst.gst_mini_object_unref((GstMiniObject *)msg);
        found = true;
    }
    m_gst.gst_object_unref(bus);
    return found;
}

/* ================================================================ encoder */

AvbStreamEncoderGStreamer::AvbStreamEncoderGStreamer() {
    char err[512];
    m_loaded = avb_gst_load(m_gst, err, sizeof(err));
    if (!m_loaded) {
        m_last_error = err;
        return;
    }
    m_gst.gst_init(nullptr, nullptr);
    m_pipe = new AvbStreamGstPipeline(m_gst);
}

AvbStreamEncoderGStreamer::~AvbStreamEncoderGStreamer() { delete m_pipe; }

const char *AvbStreamEncoderGStreamer::get_last_error() const {
    return m_last_error.empty() ? nullptr : m_last_error.c_str();
}

avb_result AvbStreamEncoderGStreamer::open_video(const AvbStreamVideoEncodeConfig &config) {
    if (!m_loaded) return AVB_ERROR_BACKEND_NOT_AVAILABLE;
    const avb_video_stream_encode_params &p = config.params;
    const avb_video_encode_params &v = p.video;
    m_is_video = true;
    m_video = config;
    if (v.input_memory != AVB_VIDEO_MEMORY_CPU) {
        m_last_error = "The GStreamer stream encoder takes CPU frames.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    int fps_n = 30, fps_d = 1;
    m_gst.gst_util_double_to_fraction(v.frame_rate, &fps_n, &fps_d);
    const std::string src_caps = strf("video/x-raw,format=%s,width=%d,height=%d,framerate=%d/%d",
                                        raw_video_format(v.input_format), v.width, v.height,
                                        fps_n, fps_d);
    std::string parse;
    if (v.codec == AVB_VIDEO_CODEC_H264)
        parse = "h264parse config-interval=-1 ! video/x-h264,stream-format=byte-stream,alignment=au";
    else if (v.codec == AVB_VIDEO_CODEC_HEVC)
        parse = "h265parse config-interval=-1 ! video/x-h265,stream-format=byte-stream,alignment=au";
    else if (v.codec == AVB_VIDEO_CODEC_AV1)
        parse = "av1parse ! video/x-av1,stream-format=obu-stream,alignment=tu";
    else
        parse = "queue";

    std::string failure = "No GStreamer encoder element is installed for the requested codec.";
    for (const VideoEncoderChoice &choice : video_encoder_choices(p)) {
        if (!element_exists(m_gst, choice.probe)) continue;
        std::string profile;
        if (choice.profile_caps) {
            if (const char *name = h264_profile_caps(p.profile))
                profile = strf(" ! video/x-h264,profile=%s", name);
            else if (p.profile == AVB_VIDEO_PROFILE_HEVC_MAIN)
                profile = " ! video/x-h265,profile=main";
        }
        const std::string description =
            "appsrc name=src format=time is-live=true do-timestamp=false max-bytes=0 ! "
            "videoconvert ! " + choice.chain + profile + " ! " + parse +
            " ! appsink name=sink sync=false";
        std::string error;
        if (m_pipe->start(description, src_caps, error)) {
            m_bitrate_property = choice.bitrate_property;
            m_bitrate_kbps = choice.kbps;
            m_output.hardware_device = choice.device;
            m_output.length_prefixed = false;
            m_last_error.clear();
            return AVB_OK;
        }
        failure = std::string(choice.probe) + ": " + error;
    }
    m_last_error = failure;
    return AVB_ERROR_OPEN_FAILED;
}

avb_result AvbStreamEncoderGStreamer::open_audio(const AvbStreamAudioEncodeConfig &config) {
    if (!m_loaded) return AVB_ERROR_BACKEND_NOT_AVAILABLE;
    const avb_audio_encode_params &a = config.params.audio;
    m_is_video = false;
    m_audio = config;
    const std::string src_caps = strf(
        "audio/x-raw,format=F32LE,layout=interleaved,rate=%d,channels=%d", a.sample_rate, a.channels);
    const char *aac[] = {"fdkaacenc", "avenc_aac", "voaacenc"};
    const char *opus[] = {"opusenc"};
    const bool is_opus = a.codec == AVB_AUDIO_CODEC_OPUS;
    const char **names = is_opus ? opus : aac;
    const int count = is_opus ? 1 : 3;
    std::string failure = is_opus ? "opusenc is not installed." : "No GStreamer AAC encoder is installed.";
    for (int i = 0; i < count; ++i) {
        if (!element_exists(m_gst, names[i])) continue;
        const std::string tail = is_opus
            ? strf("opusenc name=enc bitrate=%d frame-size=20", a.bitrate)
            : strf("%s name=enc bitrate=%d ! aacparse ! audio/mpeg,mpegversion=4,stream-format=raw",
                     names[i], a.bitrate);
        const std::string description =
            "appsrc name=src format=time is-live=true do-timestamp=false max-bytes=0 ! "
            "audioconvert ! audioresample ! " + tail + " ! appsink name=sink sync=false";
        std::string error;
        if (m_pipe->start(description, src_caps, error)) {
            m_bitrate_property = "bitrate";
            m_bitrate_kbps = false;
            m_last_error.clear();
            return AVB_OK;
        }
        failure = std::string(names[i]) + ": " + error;
    }
    m_last_error = failure;
    return AVB_ERROR_OPEN_FAILED;
}

avb_result AvbStreamEncoderGStreamer::send_video(const avb_video_frame &frame, double pts_sec,
                                                 bool force_keyframe) {
    if (!m_pipe || !m_pipe->running() || !m_is_video) return AVB_ERROR_INVALID_ARGUMENT;
    const avb_video_encode_params &v = m_video.params.video;
    if (force_keyframe && m_pipe->encoder()) {
        GstPad *pad = m_gst.gst_element_get_static_pad(m_pipe->encoder(), "src");
        if (pad) {
            m_gst.gst_pad_send_event(
                pad, m_gst.gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, TRUE, 0));
            m_gst.gst_object_unref(pad);
        }
    }
    // Repack into the GST_ROUND_UP_4 layout raw-video caps imply.
    const AvbPlaneLayout layout = avb_plane_layout(v.input_format, v.width, v.height, 4);
    const AvbPlaneLayout active = avb_plane_layout(v.input_format, v.width, v.height, 1);
    m_stage.assign(layout.total, 0);
    for (int pl = 0; pl < layout.plane_count && pl < frame.plane_count; ++pl) {
        for (int y = 0; y < active.rows[pl]; ++y) {
            std::memcpy(m_stage.data() + layout.offset[pl] + (size_t)y * layout.stride[pl],
                        frame.plane_data[pl] + (size_t)y * frame.plane_stride[pl],
                        active.stride[pl]);
        }
    }
    const GstClockTime pts = (GstClockTime)std::llround(pts_sec * GST_SECOND);
    const GstClockTime duration = (GstClockTime)std::llround(GST_SECOND / v.frame_rate);
    std::string error;
    if (!m_pipe->push(m_stage.data(), m_stage.size(), pts, duration, false, error)) {
        m_last_error = error;
        return AVB_ERROR_ENCODE_FAILED;
    }
    if (m_pipe->bus_error(error)) {
        m_last_error = error;
        return AVB_ERROR_ENCODE_FAILED;
    }
    return AVB_OK;
}

avb_result AvbStreamEncoderGStreamer::send_audio(const float *interleaved, int frames) {
    if (!m_pipe || !m_pipe->running() || m_is_video) return AVB_ERROR_INVALID_ARGUMENT;
    const avb_audio_encode_params &a = m_audio.params.audio;
    const GstClockTime pts = m_gst.gst_util_uint64_scale_round((guint64)m_audio_samples, GST_SECOND,
                                                               (guint64)a.sample_rate);
    const GstClockTime duration = m_gst.gst_util_uint64_scale_round((guint64)frames, GST_SECOND,
                                                                    (guint64)a.sample_rate);
    std::string error;
    if (!m_pipe->push(interleaved, (size_t)frames * a.channels * sizeof(float), pts, duration,
                      false, error)) {
        m_last_error = error;
        return AVB_ERROR_ENCODE_FAILED;
    }
    m_audio_samples += frames;
    if (m_pipe->bus_error(error)) {
        m_last_error = error;
        return AVB_ERROR_ENCODE_FAILED;
    }
    return AVB_OK;
}

avb_result AvbStreamEncoderGStreamer::receive(AvbStreamEncodedPacket &out) {
    if (!m_pipe || !m_pipe->running()) return m_eos ? AVB_ERROR_EOF : AVB_ERROR_INVALID_ARGUMENT;
    std::string error;
    if (m_pipe->bus_error(error)) {
        m_last_error = error;
        return AVB_ERROR_ENCODE_FAILED;
    }
    bool eos = false;
    GstSample *sample = m_pipe->pull(m_flushing ? 100 * GST_MSECOND : 0, eos);
    if (!sample) {
        if (eos) {
            m_eos = true;
            return AVB_ERROR_EOF;
        }
        return AVB_ERROR_AGAIN;
    }
    const int timescale = m_is_video ? AVB_STREAM_VIDEO_TIMESCALE : m_audio.params.audio.sample_rate;
    GstBuffer *buf = m_gst.gst_sample_get_buffer(sample);
    if (!m_is_video && m_output.extradata.empty()) {
        if (GstCaps *caps = m_gst.gst_sample_get_caps(sample)) {
            const GstStructure *s = m_gst.gst_caps_get_structure(caps, 0);
            const GValue *value = s ? m_gst.gst_structure_get_value(s, "codec_data") : nullptr;
            GstBuffer *config = value ? (GstBuffer *)m_gst.g_value_get_boxed(value) : nullptr;
            GstMapInfo cmap;
            if (config && m_gst.gst_buffer_map(config, &cmap, GST_MAP_READ)) {
                m_output.extradata.assign(cmap.data, cmap.data + cmap.size);
                m_gst.gst_buffer_unmap(config, &cmap);
            }
        }
    }
    GstMapInfo map;
    if (!buf || !m_gst.gst_buffer_map(buf, &map, GST_MAP_READ)) {
        m_gst.gst_mini_object_unref((GstMiniObject *)sample);
        m_last_error = "Mapping an encoded buffer failed.";
        return AVB_ERROR_ENCODE_FAILED;
    }
    out.data.assign(map.data, map.data + map.size);
    m_gst.gst_buffer_unmap(buf, &map);
    out.pts = clock_to_ticks(m_gst, GST_BUFFER_PTS(buf), timescale);
    out.dts = clock_to_ticks(m_gst, GST_BUFFER_DTS(buf), timescale);
    out.duration = clock_to_ticks(m_gst, GST_BUFFER_DURATION(buf), timescale);
    out.keyframe = !GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT);
    m_gst.gst_mini_object_unref((GstMiniObject *)sample);
    return AVB_OK;
}

avb_result AvbStreamEncoderGStreamer::set_bitrate(int bitrate) {
    if (!m_pipe || !m_pipe->encoder() || m_bitrate_property.empty()) {
        m_last_error = "This GStreamer encoder element cannot change its bitrate while encoding.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    const guint value = (guint)(m_bitrate_kbps ? std::max(1, bitrate / 1000) : bitrate);
    m_gst.g_object_set(m_pipe->encoder(), m_bitrate_property.c_str(), value, nullptr);
    return AVB_OK;
}

avb_result AvbStreamEncoderGStreamer::flush() {
    if (!m_pipe || !m_pipe->running()) return AVB_ERROR_INVALID_ARGUMENT;
    m_flushing = true;
    m_pipe->end_of_stream();
    return AVB_OK;
}

/* ================================================================ decoder */

AvbStreamDecoderGStreamer::AvbStreamDecoderGStreamer() {
    char err[512];
    m_loaded = avb_gst_load(m_gst, err, sizeof(err));
    if (!m_loaded) {
        m_last_error = err;
        return;
    }
    m_gst.gst_init(nullptr, nullptr);
    m_pipe = new AvbStreamGstPipeline(m_gst);
}

AvbStreamDecoderGStreamer::~AvbStreamDecoderGStreamer() { delete m_pipe; }

const char *AvbStreamDecoderGStreamer::get_last_error() const {
    return m_last_error.empty() ? nullptr : m_last_error.c_str();
}

void AvbStreamDecoderGStreamer::audio_format(int &sample_rate, int &channels) const {
    sample_rate = m_sample_rate;
    channels = m_channels;
}

avb_result AvbStreamDecoderGStreamer::start_pipeline() {
    std::string error;
    if (!m_pipe->start(m_description, m_src_caps, error)) {
        m_last_error = error;
        return AVB_ERROR_OPEN_FAILED;
    }
    m_draining = false;
    m_eos = false;
    return AVB_OK;
}

avb_result AvbStreamDecoderGStreamer::open(const AvbStreamDecodeConfig &config) {
    if (!m_loaded) return AVB_ERROR_BACKEND_NOT_AVAILABLE;
    m_config = config;
    const avb_stream_decode_params &p = config.params;
    auto first_existing = [&](std::initializer_list<const char *> names) -> const char * {
        for (const char *name : names)
            if (element_exists(m_gst, name)) return name;
        return nullptr;
    };
    const char *decoder = nullptr;
    std::string parse;
    if (p.is_video) {
        if (p.video_memory != AVB_VIDEO_MEMORY_CPU) {
            m_last_error = "The GStreamer stream decoder outputs CPU frames.";
            return AVB_ERROR_INVALID_ARGUMENT;
        }
        const bool software = p.hardware_policy == AVB_HARDWARE_DISABLED;
        switch (p.video_codec) {
            case AVB_VIDEO_CODEC_HEVC:
                m_src_caps = "video/x-h265,stream-format=byte-stream,alignment=au";
                parse = "h265parse ! ";
                decoder = software ? first_existing({"avdec_h265", "libde265dec"}) : "decodebin";
                break;
            case AVB_VIDEO_CODEC_VP8:
                m_src_caps = "video/x-vp8";
                decoder = software ? first_existing({"vp8dec", "avdec_vp8"}) : "decodebin";
                break;
            case AVB_VIDEO_CODEC_VP9:
                m_src_caps = "video/x-vp9";
                parse = "vp9parse ! ";
                decoder = software ? first_existing({"vp9dec", "avdec_vp9"}) : "decodebin";
                break;
            case AVB_VIDEO_CODEC_AV1:
                m_src_caps = "video/x-av1,stream-format=obu-stream,alignment=tu";
                parse = "av1parse ! ";
                decoder = software ? first_existing({"dav1ddec", "av1dec", "avdec_av1"}) : "decodebin";
                break;
            default:
                m_src_caps = "video/x-h264,stream-format=byte-stream,alignment=au";
                parse = "h264parse ! ";
                decoder = software ? first_existing({"avdec_h264", "openh264dec"}) : "decodebin";
                break;
        }
        if (!decoder) {
            m_last_error = "No software GStreamer decoder is installed for the requested codec.";
            return AVB_ERROR_OPEN_FAILED;
        }
        m_description = "appsrc name=src format=time is-live=true do-timestamp=false max-bytes=0 ! " +
                        parse + decoder + " ! videoconvert ! video/x-raw,format=" +
                        raw_video_format(p.video_format) + " ! appsink name=sink sync=false";
    } else {
        if (p.audio_codec == AVB_AUDIO_CODEC_OPUS) {
            m_src_caps = strf("audio/x-opus,channel-mapping-family=0,rate=48000,channels=%d",
                                p.channels);
            decoder = first_existing({"opusdec", "avdec_opus"});
        } else {
            m_src_caps = strf("audio/mpeg,mpegversion=4,stream-format=raw,rate=%d,channels=%d,"
                                "codec_data=(buffer)%s",
                                p.sample_rate, p.channels, hex(config.extradata).c_str());
            parse = "aacparse ! ";
            decoder = first_existing({"avdec_aac", "fdkaacdec", "faad"});
        }
        if (!decoder) {
            m_last_error = "No GStreamer decoder is installed for the requested audio codec.";
            return AVB_ERROR_OPEN_FAILED;
        }
        m_description = "appsrc name=src format=time is-live=true do-timestamp=false max-bytes=0 ! " +
                        parse + decoder +
                        " ! audioconvert ! audio/x-raw,format=F32LE,layout=interleaved"
                        " ! appsink name=sink sync=false";
    }
    return start_pipeline();
}

avb_result AvbStreamDecoderGStreamer::send(const AvbStreamDecodeInput &input) {
    if (!m_pipe || !m_pipe->running()) return AVB_ERROR_INVALID_ARGUMENT;
    if (!input.data) {
        m_draining = true;
        m_pipe->end_of_stream();
        return AVB_OK;
    }
    const GstClockTime pts = input.pts_sec >= 0.0
        ? (GstClockTime)std::llround(input.pts_sec * GST_SECOND) : GST_CLOCK_TIME_NONE;
    std::string error;
    const bool delta = m_config.params.is_video && !input.keyframe;
    if (!m_pipe->push(input.data, input.size, pts, GST_CLOCK_TIME_NONE, delta, error) ||
        m_pipe->bus_error(error)) {
        m_last_error = error;
        return AVB_ERROR_DECODE_FAILED;
    }
    return AVB_OK;
}

GstSample *AvbStreamDecoderGStreamer::next_sample(avb_result &result) {
    result = AVB_OK;
    if (!m_pipe || !m_pipe->running()) {
        result = m_eos ? AVB_ERROR_EOF : AVB_ERROR_INVALID_ARGUMENT;
        return nullptr;
    }
    std::string error;
    if (m_pipe->bus_error(error)) {
        m_last_error = error;
        result = AVB_ERROR_DECODE_FAILED;
        return nullptr;
    }
    bool eos = false;
    GstSample *sample = m_pipe->pull(m_draining ? 100 * GST_MSECOND : 0, eos);
    if (!sample) {
        if (eos) m_eos = true;
        result = eos ? AVB_ERROR_EOF : AVB_ERROR_AGAIN;
    }
    return sample;
}

avb_result AvbStreamDecoderGStreamer::receive_video(avb_video_frame &out) {
    if (!m_config.params.is_video) return AVB_ERROR_INVALID_ARGUMENT;
    avb_result result;
    GstSample *sample = next_sample(result);
    if (!sample) return result;
    GstCaps *caps = m_gst.gst_sample_get_caps(sample);
    const GstStructure *s = caps ? m_gst.gst_caps_get_structure(caps, 0) : nullptr;
    int w = 0, h = 0;
    if (s) {
        m_gst.gst_structure_get_int(s, "width", &w);
        m_gst.gst_structure_get_int(s, "height", &h);
    }
    GstBuffer *buf = m_gst.gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (w <= 0 || h <= 0 || !buf || !m_gst.gst_buffer_map(buf, &map, GST_MAP_READ)) {
        m_gst.gst_mini_object_unref((GstMiniObject *)sample);
        m_last_error = "The decoder returned an unreadable frame.";
        return AVB_ERROR_DECODE_FAILED;
    }
    const avb_pixel_format fmt = m_config.params.video_format;
    const AvbPlaneLayout src_layout = avb_plane_layout(fmt, w, h, 4);
    const AvbPlaneLayout dst_layout = avb_plane_layout(fmt, w, h, 1);
    size_t src_offset[AVB_MAX_PLANES];
    int src_stride[AVB_MAX_PLANES];
    for (int i = 0; i < AVB_MAX_PLANES; ++i) {
        src_offset[i] = src_layout.offset[i];
        src_stride[i] = src_layout.stride[i];
    }
    if (GstVideoMeta *meta = m_gst.gst_buffer_get_video_meta(buf)) {
        for (guint i = 0; i < meta->n_planes && i < (guint)AVB_MAX_PLANES; ++i) {
            src_offset[i] = meta->offset[i];
            src_stride[i] = meta->stride[i];
        }
    }
    m_video_buf.resize(dst_layout.total);
    bool ok = true;
    for (int pl = 0; pl < dst_layout.plane_count && ok; ++pl) {
        for (int y = 0; y < dst_layout.rows[pl]; ++y) {
            const size_t from = src_offset[pl] + (size_t)y * src_stride[pl];
            if (from + dst_layout.stride[pl] > map.size) {
                ok = false;
                break;
            }
            std::memcpy(m_video_buf.data() + dst_layout.offset[pl] + (size_t)y * dst_layout.stride[pl],
                        map.data + from, dst_layout.stride[pl]);
        }
    }
    const GstClockTime pts = GST_BUFFER_PTS(buf);
    m_gst.gst_buffer_unmap(buf, &map);
    m_gst.gst_mini_object_unref((GstMiniObject *)sample);
    if (!ok) {
        m_last_error = "A decoded frame is smaller than its caps describe.";
        return AVB_ERROR_DECODE_FAILED;
    }
    avb::detail::init_cpu_frame(out, w, h, fmt,
                                GST_CLOCK_TIME_IS_VALID(pts) ? (double)pts / GST_SECOND : -1.0);
    out.plane_count = dst_layout.plane_count;
    for (int i = 0; i < dst_layout.plane_count; ++i) {
        out.plane_data[i] = m_video_buf.data() + dst_layout.offset[i];
        out.plane_stride[i] = dst_layout.stride[i];
        out.plane_offset[i] = (int)dst_layout.offset[i];
    }
    out.data = out.plane_data[0];
    out.stride = out.plane_stride[0];
    out.data_size = (int)m_video_buf.size();
    return AVB_OK;
}

avb_result AvbStreamDecoderGStreamer::receive_audio(std::vector<float> &out, int &frames,
                                                    double &pts_sec) {
    frames = 0;
    pts_sec = -1.0;
    if (m_config.params.is_video) return AVB_ERROR_INVALID_ARGUMENT;
    avb_result result;
    GstSample *sample = next_sample(result);
    if (!sample) return result;
    GstCaps *caps = m_gst.gst_sample_get_caps(sample);
    const GstStructure *s = caps ? m_gst.gst_caps_get_structure(caps, 0) : nullptr;
    int rate = 0, channels = 0;
    if (s) {
        m_gst.gst_structure_get_int(s, "rate", &rate);
        m_gst.gst_structure_get_int(s, "channels", &channels);
    }
    GstBuffer *buf = m_gst.gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (rate <= 0 || channels <= 0 || !buf || !m_gst.gst_buffer_map(buf, &map, GST_MAP_READ)) {
        m_gst.gst_mini_object_unref((GstMiniObject *)sample);
        m_last_error = "The decoder returned unreadable audio.";
        return AVB_ERROR_DECODE_FAILED;
    }
    m_sample_rate = rate;
    m_channels = channels;
    frames = (int)(map.size / (sizeof(float) * channels));
    out.resize((size_t)frames * channels);
    std::memcpy(out.data(), map.data, out.size() * sizeof(float));
    const GstClockTime pts = GST_BUFFER_PTS(buf);
    m_gst.gst_buffer_unmap(buf, &map);
    m_gst.gst_mini_object_unref((GstMiniObject *)sample);
    if (GST_CLOCK_TIME_IS_VALID(pts)) pts_sec = (double)pts / GST_SECOND;
    return frames > 0 ? AVB_OK : AVB_ERROR_AGAIN;
}

avb_result AvbStreamDecoderGStreamer::reset() {
    if (!m_pipe) return AVB_ERROR_INVALID_ARGUMENT;
    // Rebuilding drops every queued buffer, which is what a reset means.
    return start_pipeline();
}
