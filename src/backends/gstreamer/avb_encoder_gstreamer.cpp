#include "avb_encoder_gstreamer.hpp"
#include "../../avb_video_codec_registry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

static inline int round_up_4(int x) { return (x + 3) & ~3; }

static bool avb_is_compressed_format(avb_pixel_format fmt) {
    return fmt == AVB_PIXEL_FORMAT_BC1_RGBA ||
           fmt == AVB_PIXEL_FORMAT_BC3_RGBA ||
           fmt == AVB_PIXEL_FORMAT_BC4_R ||
           fmt == AVB_PIXEL_FORMAT_BC5_RG ||
           fmt == AVB_PIXEL_FORMAT_BC7_RGBA;
}

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
    if (m_custom_video_encoder) {
        if (m_custom_video_encoder->close && m_custom_video_ctx)
            m_custom_video_encoder->close(m_custom_video_ctx);
        m_custom_video_encoder = nullptr;
        m_custom_video_ctx = nullptr;
        m_custom_video_stream = {};
    }
    if (m_pipeline) {
        m_gst.gst_element_set_state(m_pipeline, GST_STATE_NULL);
        m_gst.gst_object_unref(m_pipeline);
    m_pipeline = nullptr;
    }
    m_custom_video = false;
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

    // Pick the muxer from the file extension. The codec must be compatible with
    // the chosen container (e.g. VP9/Opus in .webm/.mkv); incompatible pairs are
    // surfaced as a pipeline error.
    std::string p(path);
    auto ends_with = [&](const char *s) {
        size_t n = strlen(s);
        return p.size() >= n &&
               std::equal(p.end() - n, p.end(), s,
                          [](char a, char b){ return ::tolower(a) == b; });
    };
    const char *mux = ends_with(".webm") ? "webmmux"
                    : ends_with(".mkv")  ? "matroskamux"
                    : ends_with(".mov")  ? "qtmux"
                                         : "mp4mux";

    // Video codec -> encoder element (with its bitrate property/units).
    char venc[128] = "x264enc speed-preset=veryfast";
    const char *vfmt = "BGRA";
    std::string custom_vcaps;
    if (options.video.enable) {
        switch (options.video.input_format) {
            case AVB_PIXEL_FORMAT_RGBA8: m_input_format = AVB_PIXEL_FORMAT_RGBA8; vfmt = "RGBA"; break;
            case AVB_PIXEL_FORMAT_NV12:  m_input_format = AVB_PIXEL_FORMAT_NV12;  vfmt = "NV12"; break;
            case AVB_PIXEL_FORMAT_I420:  m_input_format = AVB_PIXEL_FORMAT_I420;  vfmt = "I420"; break;
            case AVB_PIXEL_FORMAT_BC1_RGBA:
            case AVB_PIXEL_FORMAT_BC3_RGBA:
            case AVB_PIXEL_FORMAT_BC4_R:
            case AVB_PIXEL_FORMAT_BC5_RG:
            case AVB_PIXEL_FORMAT_BC7_RGBA:
                m_input_format = options.video.input_format;
                break;
            case AVB_PIXEL_FORMAT_BGRA8:
            case AVB_PIXEL_FORMAT_UNKNOWN:
            default:                     m_input_format = AVB_PIXEL_FORMAT_BGRA8; vfmt = "BGRA"; break;
        }
        int kbps = options.video.bitrate / 1000;
        switch (options.video.codec) {
            case AVB_CODEC_AUTO:
            case AVB_CODEC_H264:
                if (kbps > 0) snprintf(venc, sizeof(venc), "x264enc speed-preset=veryfast bitrate=%d", kbps);
                else          snprintf(venc, sizeof(venc), "x264enc speed-preset=veryfast");
                break;
            case AVB_CODEC_HEVC:
                if (kbps > 0) snprintf(venc, sizeof(venc), "x265enc speed-preset=veryfast bitrate=%d", kbps);
                else          snprintf(venc, sizeof(venc), "x265enc speed-preset=veryfast");
                break;
            case AVB_CODEC_VP9:
                if (options.video.bitrate > 0) snprintf(venc, sizeof(venc), "vp9enc deadline=1 target-bitrate=%d", options.video.bitrate);
                else          snprintf(venc, sizeof(venc), "vp9enc deadline=1");
                break;
            case AVB_CODEC_HAP:
                break;
            default:
                m_last_error = "Invalid video codec (use AUTO/H264/HEVC/VP9/HAP).";
                return AVB_ERROR_INVALID_ARGUMENT;
        }
        if (options.video.width <= 0 || options.video.height <= 0) {
            m_last_error = "Video width/height must be positive.";
            return AVB_ERROR_INVALID_ARGUMENT;
        }
        m_width      = options.video.width;
        m_height     = options.video.height;
        m_frame_rate = options.video.frame_rate > 0 ? options.video.frame_rate : 30.0;
        m_fps_n      = std::max(1L, std::lround(m_frame_rate));

        avb_video_encode_info custom_info{};
        custom_info.width = m_width;
        custom_info.height = m_height;
        custom_info.frame_rate = m_frame_rate;
        custom_info.input_format = m_input_format;
        custom_info.codec = options.video.codec;
        custom_info.bitrate = options.video.bitrate;
        const avb_video_encoder_plugin *plugin =
            avb_find_video_encoder_plugin(custom_info);
        if (plugin) {
            void *ctx = nullptr;
            avb_encoded_video_stream stream{};
            avb_result cres = plugin->open(&ctx, &custom_info, &stream);
            if (cres != AVB_OK) {
                m_last_error = "Custom video encoder failed to open.";
                return cres;
            }
            m_custom_video_encoder = plugin;
            m_custom_video_ctx = ctx;
            m_custom_video_stream = stream;
            m_custom_video = true;
            if (stream.gst_caps && stream.gst_caps[0]) {
                custom_vcaps = stream.gst_caps;
            } else if (stream.codec == AVB_CODEC_HAP || options.video.codec == AVB_CODEC_HAP) {
                custom_vcaps = "video/x-hap,width=%d,height=%d,framerate=%d/1";
            } else {
                m_last_error = "Custom GStreamer video encoder requires gst_caps.";
                return AVB_ERROR_INVALID_ARGUMENT;
            }
        }
        if (!m_custom_video && options.video.codec == AVB_CODEC_HAP) {
            m_last_error = "HAP encoding requires a registered custom video encoder.";
            return AVB_ERROR_INVALID_ARGUMENT;
        }
        if (!m_custom_video && avb_is_compressed_format(m_input_format)) {
            m_last_error = "Compressed video input requires a registered custom video encoder.";
            return AVB_ERROR_INVALID_ARGUMENT;
        }
    }

    // Audio codec -> encoder element.
    char aenc[128] = "avenc_aac";
    if (options.audio.enable) {
        switch (options.audio.codec) {
            case AVB_CODEC_AUTO:
            case AVB_CODEC_AAC:
                if (options.audio.bitrate > 0) snprintf(aenc, sizeof(aenc), "avenc_aac bitrate=%d", options.audio.bitrate);
                else          snprintf(aenc, sizeof(aenc), "avenc_aac");
                break;
            case AVB_CODEC_OPUS:
                if (options.audio.bitrate > 0) snprintf(aenc, sizeof(aenc), "opusenc bitrate=%d", options.audio.bitrate);
                else          snprintf(aenc, sizeof(aenc), "opusenc");
                break;
            default:
                m_last_error = "Invalid audio codec (use AUTO/AAC/OPUS).";
                return AVB_ERROR_INVALID_ARGUMENT;
        }
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
        if (m_custom_video) {
            n += snprintf(desc + n, sizeof(desc) - n,
                "appsrc name=vsrc is-live=false format=time max-bytes=0 ! queue ! mux. ");
        } else {
            n += snprintf(desc + n, sizeof(desc) - n,
                "appsrc name=vsrc is-live=false format=time max-bytes=0 ! videoconvert ! "
                "%s ! queue ! mux. ", venc);
        }
    }
    if (options.audio.enable) {
        n += snprintf(desc + n, sizeof(desc) - n,
            "appsrc name=asrc is-live=false format=time max-bytes=0 ! audioconvert ! "
            "audioresample ! %s ! queue ! mux. ", aenc);
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
        if (m_custom_video) {
            if (custom_vcaps.find("%d") != std::string::npos) {
                snprintf(caps_str, sizeof(caps_str), custom_vcaps.c_str(),
                         m_width, m_height, m_fps_n);
            } else {
                snprintf(caps_str, sizeof(caps_str), "%s", custom_vcaps.c_str());
            }
        } else {
            snprintf(caps_str, sizeof(caps_str),
                     "video/x-raw,format=%s,width=%d,height=%d,framerate=%d/1",
                     vfmt, m_width, m_height, m_fps_n);
        }
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
    if (m_custom_video) {
        avb_encoded_packet packet{};
        avb_result res = m_custom_video_encoder->encode_frame(
            m_custom_video_ctx, &frame, pts_sec, &packet);
        if (res != AVB_OK) return res;
        double fallback_pts = pts_sec >= 0.0 ? pts_sec
                           : frame.pts_sec >= 0.0 ? frame.pts_sec
                           : (double)m_video_index / m_frame_rate;
        res = write_custom_video_packet(packet, fallback_pts);
        if (m_custom_video_encoder->release_packet)
            m_custom_video_encoder->release_packet(m_custom_video_ctx, &packet);
        if (res == AVB_OK) m_video_index++;
        return res;
    }
    if (frame.format != m_input_format) {
        m_last_error = "Frame pixel format does not match configured input_format.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }

    double pts = pts_sec >= 0.0      ? pts_sec
               : frame.pts_sec >= 0.0 ? frame.pts_sec
               : (double)m_video_index / m_frame_rate;

    // Repack into the tightly-packed (GST_ROUND_UP_4 stride) layout appsrc wants.
    //   BGRA/RGBA: 1 plane; NV12: 2 (Y, CbCr); I420: 3 (Y, Cb, Cr).
    int    dst_stride[AVB_MAX_PLANES] = {0, 0, 0};
    int    plane_rows[AVB_MAX_PLANES] = {0, 0, 0};
    int    plane_count = 1;
    switch (m_input_format) {
        case AVB_PIXEL_FORMAT_NV12:
            plane_count = 2;
            dst_stride[0] = round_up_4(m_width);     plane_rows[0] = m_height;
            dst_stride[1] = round_up_4(m_width);     plane_rows[1] = m_height / 2;
            break;
        case AVB_PIXEL_FORMAT_I420:
            plane_count = 3;
            dst_stride[0] = round_up_4(m_width);     plane_rows[0] = m_height;
            dst_stride[1] = round_up_4(m_width / 2); plane_rows[1] = m_height / 2;
            dst_stride[2] = round_up_4(m_width / 2); plane_rows[2] = m_height / 2;
            break;
        default:
            plane_count = 1;
            dst_stride[0] = round_up_4(m_width * 4); plane_rows[0] = m_height;
            break;
    }

    size_t plane_off[AVB_MAX_PLANES] = {0, 0, 0};
    size_t total = 0;
    for (int pl = 0; pl < plane_count; ++pl) {
        plane_off[pl] = total;
        total += (size_t)dst_stride[pl] * plane_rows[pl];
    }

    m_stage.resize(total);
    for (int pl = 0; pl < plane_count && pl < frame.plane_count; ++pl) {
        int copy = std::min(frame.plane_stride[pl], dst_stride[pl]);
        for (int y = 0; y < plane_rows[pl]; ++y) {
            memcpy(m_stage.data() + plane_off[pl] + (size_t)y * dst_stride[pl],
                   frame.plane_data[pl] + (size_t)y * frame.plane_stride[pl],
                   copy);
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

avb_result AvbEncoderGStreamer::write_custom_video_packet(
    avb_encoded_packet &packet,
    double fallback_pts
) {
    if (!packet.data || packet.size <= 0) return AVB_ERROR_INVALID_ARGUMENT;

    GstBuffer *buf = m_gst.gst_buffer_new_allocate(nullptr, packet.size, nullptr);
    if (!buf) {
        m_last_error = "gst_buffer_new_allocate (custom video) failed.";
        return AVB_ERROR_ENCODE_FAILED;
    }
    m_gst.gst_buffer_fill(buf, 0, packet.data, packet.size);
    double pts = packet.pts_sec >= 0.0 ? packet.pts_sec : fallback_pts;
    double dur = packet.duration_sec > 0.0 ? packet.duration_sec : 1.0 / m_frame_rate;
    buf->pts = (GstClockTime)std::llround(pts * GST_SECOND);
    buf->dts = packet.dts >= 0
        ? (GstClockTime)packet.dts
        : buf->pts;
    buf->duration = (GstClockTime)std::llround(dur * GST_SECOND);
    if (!packet.keyframe)
        GST_BUFFER_FLAG_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT);

    GstFlowReturn fr = m_gst.gst_app_src_push_buffer((GstAppSrc *)m_vsrc, buf);
    if (fr != GST_FLOW_OK) {
        if (check_bus_error() == AVB_OK) m_last_error = "appsrc push (custom video) failed.";
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

    if (m_has_video) {
        if (m_custom_video) {
            while (m_custom_video_encoder->flush) {
                avb_encoded_packet packet{};
                avb_result r = m_custom_video_encoder->flush(m_custom_video_ctx, &packet);
                if (r == AVB_ERROR_EOF || r == AVB_ERROR_AGAIN) break;
                if (r != AVB_OK) return r;
                r = write_custom_video_packet(packet, (double)m_video_index / m_frame_rate);
                if (m_custom_video_encoder->release_packet)
                    m_custom_video_encoder->release_packet(m_custom_video_ctx, &packet);
                if (r != AVB_OK) return r;
                m_video_index++;
            }
        }
        m_gst.gst_app_src_end_of_stream((GstAppSrc *)m_vsrc);
    }
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
