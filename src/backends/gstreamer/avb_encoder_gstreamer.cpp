#include "avb_encoder_gstreamer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

static inline int round_up_4(int x) { return (x + 3) & ~3; }

AvbEncoderGStreamer::AvbEncoderGStreamer() {
    char err_buf[512];
    m_libs_loaded = avb_gst_load(m_gst, err_buf, sizeof(err_buf));
    if (!m_libs_loaded) { m_last_error = err_buf; return; }
    m_gst.gst_init(nullptr, nullptr);
}

AvbEncoderGStreamer::~AvbEncoderGStreamer() { close_internal(); }

const char *AvbEncoderGStreamer::get_last_error() const {
    return m_last_error.empty() ? nullptr : m_last_error.c_str();
}

void AvbEncoderGStreamer::close_internal() {
    if (!m_libs_loaded) return;
    if (m_vsrc) { m_gst.gst_object_unref(m_vsrc); m_vsrc = nullptr; }
    if (m_asrc) { m_gst.gst_object_unref(m_asrc); m_asrc = nullptr; }
    if (m_pipeline) {
        m_gst.gst_element_set_state(m_pipeline, GST_STATE_NULL);
        m_gst.gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
    }
}

// Poll the bus (non-blocking) for an ERROR posted by the pipeline and turn it
// into m_last_error. Returns ENCODE_FAILED if one was found, OK otherwise.
avb_result AvbEncoderGStreamer::check_bus_error() {
    GstBus *bus = m_gst.gst_element_get_bus(m_pipeline);
    if (!bus) return AVB_OK;
    GstMessage *msg = m_gst.gst_bus_timed_pop_filtered(bus, 0, GST_MESSAGE_ERROR);
    avb_result r = AVB_OK;
    if (msg) {
        GError *err = nullptr; gchar *dbg = nullptr;
        m_gst.gst_message_parse_error(msg, &err, &dbg);
        m_last_error = err && err->message ? err->message : "GStreamer encode error.";
        if (err) m_gst.g_error_free(err);
        if (dbg) m_gst.g_free(dbg);
        m_gst.gst_mini_object_unref((GstMiniObject *)msg);
        r = AVB_ERROR_ENCODE_FAILED;
    }
    m_gst.gst_object_unref(bus);
    return r;
}

avb_result AvbEncoderGStreamer::open(const char *path, const avb_encode_options &options) {
    if (!m_libs_loaded) return AVB_ERROR_BACKEND_NOT_AVAILABLE;
    close_internal();

    if (!options.video.enable && !options.audio.enable) {
        m_last_error = "Encoder requires at least one of video/audio enabled.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }

    // Pick the muxer from the file extension (.mov -> qtmux, else mp4mux).
    std::string p(path);
    std::string ext = p.size() >= 4 ? p.substr(p.size() - 4) : "";
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    const char *mux = (ext == ".mov") ? "qtmux" : "mp4mux";

    const char *vfmt = "BGRA";
    if (options.video.enable) {
        switch (options.video.input_format) {
            case AVB_PIXEL_FORMAT_RGBA8: m_input_format = AVB_PIXEL_FORMAT_RGBA8; vfmt = "RGBA"; break;
            case AVB_PIXEL_FORMAT_NV12:  m_input_format = AVB_PIXEL_FORMAT_NV12;  vfmt = "NV12"; break;
            case AVB_PIXEL_FORMAT_BGRA8:
            case AVB_PIXEL_FORMAT_UNKNOWN:
            default:                     m_input_format = AVB_PIXEL_FORMAT_BGRA8; vfmt = "BGRA"; break;
        }
        if (options.video.width <= 0 || options.video.height <= 0) {
            m_last_error = "Video width/height must be positive.";
            return AVB_ERROR_INVALID_ARGUMENT;
        }
        m_width      = options.video.width;
        m_height     = options.video.height;
        m_frame_rate = options.video.frame_rate > 0 ? options.video.frame_rate : 30.0;
        m_fps_n      = std::max(1L, std::lround(m_frame_rate));
    }
    if (options.audio.enable) {
        if (options.audio.sample_rate <= 0 || options.audio.channels <= 0) {
            m_last_error = "Audio sample_rate/channels must be positive.";
            return AVB_ERROR_INVALID_ARGUMENT;
        }
        m_sample_rate = options.audio.sample_rate;
        m_channels    = options.audio.channels;
    }

    // Build the pipeline: muxer + filesink, plus an appsrc->encoder chain per
    // enabled track. max-bytes=0 makes each appsrc queue unbounded so pushing
    // one track far ahead of the other never blocks (and never deadlocks the
    // muxer), mirroring the FFmpeg encoder's unbounded audio FIFO.
    char desc[1024];
    int n = snprintf(desc, sizeof(desc), "%s name=mux ! filesink name=sink ", mux);
    if (options.video.enable) {
        n += snprintf(desc + n, sizeof(desc) - n,
            "appsrc name=vsrc is-live=false format=time max-bytes=0 ! videoconvert ! "
            "x264enc speed-preset=veryfast");
        if (options.video.bitrate > 0)
            n += snprintf(desc + n, sizeof(desc) - n, " bitrate=%d", options.video.bitrate / 1000);
        n += snprintf(desc + n, sizeof(desc) - n, " ! queue ! mux. ");
    }
    if (options.audio.enable) {
        n += snprintf(desc + n, sizeof(desc) - n,
            "appsrc name=asrc is-live=false format=time max-bytes=0 ! audioconvert ! "
            "audioresample ! avenc_aac");
        if (options.audio.bitrate > 0)
            n += snprintf(desc + n, sizeof(desc) - n, " bitrate=%d", options.audio.bitrate);
        n += snprintf(desc + n, sizeof(desc) - n, " ! queue ! mux. ");
    }

    GError *err = nullptr;
    m_pipeline = m_gst.gst_parse_launch(desc, &err);
    if (!m_pipeline) {
        snprintf(desc, sizeof(desc), "gst_parse_launch failed: %s",
                 err && err->message ? err->message : "unknown");
        m_last_error = desc;
        m_gst.g_clear_error(&err);
        return AVB_ERROR_OPEN_FAILED;
    }
    m_gst.g_clear_error(&err); // parse_launch can warn but still succeed

    GstElement *sink = m_gst.gst_bin_get_by_name((GstBin *)m_pipeline, "sink");
    if (sink) {
        m_gst.g_object_set(sink, "location", path, nullptr);
        m_gst.gst_object_unref(sink);
    }

    if (options.video.enable) {
        m_vsrc = m_gst.gst_bin_get_by_name((GstBin *)m_pipeline, "vsrc");
        char caps_str[256];
        snprintf(caps_str, sizeof(caps_str),
                 "video/x-raw,format=%s,width=%d,height=%d,framerate=%d/1",
                 vfmt, m_width, m_height, m_fps_n);
        GstCaps *caps = m_gst.gst_caps_from_string(caps_str);
        m_gst.g_object_set(m_vsrc, "caps", caps, nullptr);
        m_gst.gst_mini_object_unref((GstMiniObject *)caps);
        m_has_video = true;
    }
    if (options.audio.enable) {
        m_asrc = m_gst.gst_bin_get_by_name((GstBin *)m_pipeline, "asrc");
        char caps_str[256];
        snprintf(caps_str, sizeof(caps_str),
                 "audio/x-raw,format=F32LE,layout=interleaved,channels=%d,rate=%d",
                 m_channels, m_sample_rate);
        GstCaps *caps = m_gst.gst_caps_from_string(caps_str);
        m_gst.g_object_set(m_asrc, "caps", caps, nullptr);
        m_gst.gst_mini_object_unref((GstMiniObject *)caps);
        m_has_audio = true;
    }

    if (m_gst.gst_element_set_state(m_pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        check_bus_error();
        if (m_last_error.empty()) m_last_error = "Encoder pipeline failed to start.";
        return AVB_ERROR_OPEN_FAILED;
    }

    return AVB_OK;
}

avb_result AvbEncoderGStreamer::write_video(const avb_video_frame &frame, double pts_sec) {
    if (!m_has_video) return AVB_ERROR_INVALID_ARGUMENT;
    if (frame.format != m_input_format) {
        m_last_error = "Frame pixel format does not match configured input_format.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }

    double pts = pts_sec >= 0.0      ? pts_sec
               : frame.pts_sec >= 0.0 ? frame.pts_sec
               : (double)m_video_index / m_frame_rate;

    // Repack into the tightly-packed (GST_ROUND_UP_4 stride) layout appsrc wants.
    bool nv12 = (m_input_format == AVB_PIXEL_FORMAT_NV12);
    int  ds0  = nv12 ? round_up_4(m_width) : round_up_4(m_width * 4);
    size_t y_size = (size_t)ds0 * m_height;
    size_t total  = y_size;
    int    ds1 = 0; size_t uv_size = 0;
    if (nv12) { ds1 = round_up_4(m_width); uv_size = (size_t)ds1 * (m_height / 2); total += uv_size; }

    m_stage.resize(total);
    for (int y = 0; y < m_height; ++y) {
        memcpy(m_stage.data() + (size_t)y * ds0,
               frame.plane_data[0] + (size_t)y * frame.plane_stride[0],
               std::min(frame.plane_stride[0], ds0));
    }
    if (nv12 && frame.plane_count >= 2) {
        for (int y = 0; y < m_height / 2; ++y) {
            memcpy(m_stage.data() + y_size + (size_t)y * ds1,
                   frame.plane_data[1] + (size_t)y * frame.plane_stride[1],
                   std::min(frame.plane_stride[1], ds1));
        }
    }

    GstBuffer *buf = m_gst.gst_buffer_new_allocate(nullptr, total, nullptr);
    if (!buf) { m_last_error = "gst_buffer_new_allocate (video) failed."; return AVB_ERROR_ENCODE_FAILED; }
    m_gst.gst_buffer_fill(buf, 0, m_stage.data(), total);
    buf->pts      = (GstClockTime)std::llround(pts * GST_SECOND);
    buf->duration = (GstClockTime)(GST_SECOND / m_frame_rate);

    GstFlowReturn fr = m_gst.gst_app_src_push_buffer((GstAppSrc *)m_vsrc, buf);
    m_video_index++;
    if (fr != GST_FLOW_OK) {
        if (check_bus_error() == AVB_OK) m_last_error = "appsrc push (video) failed.";
        return AVB_ERROR_ENCODE_FAILED;
    }
    return AVB_OK;
}

avb_result AvbEncoderGStreamer::write_audio_f32(const float *src_interleaved, int frames) {
    if (!m_has_audio) return AVB_ERROR_INVALID_ARGUMENT;

    size_t bytes = (size_t)frames * m_channels * sizeof(float);
    GstBuffer *buf = m_gst.gst_buffer_new_allocate(nullptr, bytes, nullptr);
    if (!buf) { m_last_error = "gst_buffer_new_allocate (audio) failed."; return AVB_ERROR_ENCODE_FAILED; }
    m_gst.gst_buffer_fill(buf, 0, src_interleaved, bytes);
    buf->pts      = (GstClockTime)((double)m_audio_samples / m_sample_rate * GST_SECOND);
    buf->duration = (GstClockTime)((double)frames / m_sample_rate * GST_SECOND);
    m_audio_samples += frames;

    GstFlowReturn fr = m_gst.gst_app_src_push_buffer((GstAppSrc *)m_asrc, buf);
    if (fr != GST_FLOW_OK) {
        if (check_bus_error() == AVB_OK) m_last_error = "appsrc push (audio) failed.";
        return AVB_ERROR_ENCODE_FAILED;
    }
    return AVB_OK;
}

avb_result AvbEncoderGStreamer::finish() {
    if (!m_pipeline) return AVB_ERROR_INVALID_ARGUMENT;
    if (m_finished) return AVB_OK;

    if (m_has_video) m_gst.gst_app_src_end_of_stream((GstAppSrc *)m_vsrc);
    if (m_has_audio) m_gst.gst_app_src_end_of_stream((GstAppSrc *)m_asrc);

    // Wait for the muxer/filesink to finalize the file (EOS) or report an error.
    GstBus *bus = m_gst.gst_element_get_bus(m_pipeline);
    GstMessage *msg = m_gst.gst_bus_timed_pop_filtered(
        bus, 30 * GST_SECOND,
        (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));

    avb_result res = AVB_OK;
    if (!msg) {
        m_last_error = "Timed out waiting for encoder to finish.";
        res = AVB_ERROR_ENCODE_FAILED;
    } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        GError *err = nullptr; gchar *dbg = nullptr;
        m_gst.gst_message_parse_error(msg, &err, &dbg);
        m_last_error = err && err->message ? err->message : "Encoder error at finish.";
        if (err) m_gst.g_error_free(err);
        if (dbg) m_gst.g_free(dbg);
        res = AVB_ERROR_ENCODE_FAILED;
    }
    if (msg) m_gst.gst_mini_object_unref((GstMiniObject *)msg);
    m_gst.gst_object_unref(bus);

    m_gst.gst_element_set_state(m_pipeline, GST_STATE_NULL);
    m_finished = true;
    return res;
}
