#include "avb_backend_gstreamer.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

// playbin "flags" bits (from playbin's GstPlayFlags; values are stable ABI).
// Defined locally so we don't depend on the flags enum being in public headers.
enum {
    AVB_GST_PLAY_FLAG_VIDEO = (1 << 0),
    AVB_GST_PLAY_FLAG_AUDIO = (1 << 1),
};

static inline int round_up_4(int x) { return (x + 3) & ~3; }

// Map a GStreamer source-stream caps name to the same short codec name the
// ffmpeg backend reports, so avb_*_info::codec_name is consistent across
// backends. Unknown types fall back to the caps media type minus prefix.
static std::string caps_to_codec_name(const AvbGstFuncs &gst, const GstCaps *caps) {
    if (!caps) return "";
    const GstStructure *s = gst.gst_caps_get_structure(caps, 0);
    if (!s) return "";
    const char *name = gst.gst_structure_get_name(s);
    if (!name) return "";

    auto get_int = [&](const char *field, int def) {
        int v = def;
        gst.gst_structure_get_int(s, field, &v);
        return v;
    };

    if (strcmp(name, "audio/mpeg") == 0) {
        int ver = get_int("mpegversion", 0);
        if (ver == 4 || ver == 2) return "aac";
        if (ver == 1) {
            switch (get_int("layer", 3)) {
                case 1:  return "mp1";
                case 2:  return "mp2";
                default: return "mp3";
            }
        }
        return "mpeg";
    }
    if (strcmp(name, "video/mpeg") == 0) {
        switch (get_int("mpegversion", 0)) {
            case 4:  return "mpeg4";
            case 2:  return "mpeg2video";
            case 1:  return "mpeg1video";
            default: return "mpeg";
        }
    }
    if (strcmp(name, "audio/x-raw") == 0)  return "pcm";
    if (strcmp(name, "audio/x-vorbis") == 0) return "vorbis";
    if (strcmp(name, "audio/x-opus") == 0) return "opus";
    if (strcmp(name, "audio/x-flac") == 0) return "flac";
    if (strcmp(name, "audio/x-ac3") == 0)  return "ac3";
    if (strcmp(name, "audio/x-eac3") == 0) return "eac3";
    if (strcmp(name, "audio/x-alac") == 0) return "alac";
    if (strcmp(name, "video/x-h264") == 0) return "h264";
    if (strcmp(name, "video/x-h265") == 0) return "hevc";
    if (strcmp(name, "video/x-vp8") == 0)  return "vp8";
    if (strcmp(name, "video/x-vp9") == 0)  return "vp9";
    if (strcmp(name, "video/x-av1") == 0)  return "av1";
    if (strcmp(name, "video/x-prores") == 0) return "prores";
    if (strcmp(name, "image/jpeg") == 0)   return "mjpeg";

    // Fallback: strip a "video/x-" / "audio/x-" prefix if present.
    const char *slash = strchr(name, '/');
    if (slash) {
        const char *rest = slash + 1;
        if (strncmp(rest, "x-", 2) == 0) rest += 2;
        return rest;
    }
    return name;
}

AvbBackendGStreamer::AvbBackendGStreamer() {
    char err_buf[512];
    m_libs_loaded = avb_gst_load(m_gst, err_buf, sizeof(err_buf));
    if (!m_libs_loaded) {
        m_last_error = err_buf;
        return;
    }
    m_gst.gst_init(nullptr, nullptr);
}

AvbBackendGStreamer::~AvbBackendGStreamer() {
    close_internal();
}

const char *AvbBackendGStreamer::get_backend_name() const { return "gstreamer"; }
const char *AvbBackendGStreamer::get_last_error() const {
    return m_last_error.empty() ? nullptr : m_last_error.c_str();
}

void AvbBackendGStreamer::set_error(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    m_last_error = buf;
}

void AvbBackendGStreamer::close_internal() {
    if (!m_libs_loaded) return;

    if (m_audio_sink) { m_gst.gst_object_unref(m_audio_sink); m_audio_sink = nullptr; }
    if (m_video_sink) { m_gst.gst_object_unref(m_video_sink); m_video_sink = nullptr; }
    if (m_pipeline) {
        m_gst.gst_element_set_state(m_pipeline, GST_STATE_NULL);
        m_gst.gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
    }

    m_audio_buf.clear();
    m_audio_buf_pos = 0;
    m_video_out_buf.clear();
    m_out_sample_rate = 0;
    m_out_channels    = 0;
    m_width = m_height = 0;
    m_frame_rate = 0.0;
    m_duration   = 0.0;
    m_audio_eof  = false;
    m_seek_target = -1.0;
    m_audio_codec_name.clear();
    m_video_codec_name.clear();
}

void AvbBackendGStreamer::discover_codec_names(const char *uri) {
    GError *err = nullptr;
    GstDiscoverer *disc = m_gst.gst_discoverer_new(5 * GST_SECOND, &err);
    if (!disc) { m_gst.g_clear_error(&err); return; }

    GstDiscovererInfo *info = m_gst.gst_discoverer_discover_uri(disc, uri, &err);
    if (info) {
        GList *al = m_gst.gst_discoverer_info_get_audio_streams(info);
        if (al) {
            GstCaps *caps = m_gst.gst_discoverer_stream_info_get_caps(
                (GstDiscovererStreamInfo *)al->data);
            m_audio_codec_name = caps_to_codec_name(m_gst, caps);
            if (caps) m_gst.gst_mini_object_unref((GstMiniObject *)caps);
            m_gst.gst_discoverer_stream_info_list_free(al);
        }
        GList *vl = m_gst.gst_discoverer_info_get_video_streams(info);
        if (vl) {
            GstCaps *caps = m_gst.gst_discoverer_stream_info_get_caps(
                (GstDiscovererStreamInfo *)vl->data);
            m_video_codec_name = caps_to_codec_name(m_gst, caps);
            if (caps) m_gst.gst_mini_object_unref((GstMiniObject *)caps);
            m_gst.gst_discoverer_stream_info_list_free(vl);
        }
        m_gst.g_object_unref(info);
    }
    m_gst.g_clear_error(&err);
    m_gst.g_object_unref(disc);
}

avb_result AvbBackendGStreamer::open_file(const char *path, const avb_decode_options &options) {
    if (!m_libs_loaded) return AVB_ERROR_BACKEND_NOT_AVAILABLE;

    close_internal();

    if (!options.enable_audio && !options.enable_video) {
        set_error("Neither audio nor video requested.");
        return AVB_ERROR_INVALID_ARGUMENT;
    }

    GError *err = nullptr;
    gchar *uri = m_gst.gst_filename_to_uri(path, &err);
    if (!uri) {
        set_error("Failed to build URI for '%s'.", path);
        m_gst.g_clear_error(&err);
        return AVB_ERROR_OPEN_FAILED;
    }

    m_pipeline = m_gst.gst_element_factory_make("playbin", nullptr);
    if (!m_pipeline) {
        set_error("Failed to create playbin (is the GStreamer base plugin set installed?).");
        m_gst.g_free(uri);
        return AVB_ERROR_OPEN_FAILED;
    }
    m_gst.g_object_set(m_pipeline, "uri", uri, nullptr);

    // Resolve requested output video pixel format.
    const char *vfmt = "BGRA";
    switch (options.video_format) {
        case AVB_PIXEL_FORMAT_RGBA8: m_video_format = AVB_PIXEL_FORMAT_RGBA8; vfmt = "RGBA"; break;
        case AVB_PIXEL_FORMAT_NV12:  m_video_format = AVB_PIXEL_FORMAT_NV12;  vfmt = "NV12"; break;
        case AVB_PIXEL_FORMAT_I420:  m_video_format = AVB_PIXEL_FORMAT_I420;  vfmt = "I420"; break;
        case AVB_PIXEL_FORMAT_BGRA8:
        case AVB_PIXEL_FORMAT_UNKNOWN:
        default:                     m_video_format = AVB_PIXEL_FORMAT_BGRA8; vfmt = "BGRA"; break;
    }

    int flags = 0;

    // Build the audio sink bin: convert/resample to interleaved F32LE.
    if (options.enable_audio) {
        m_req_sample_rate = options.audio_sample_rate;
        m_req_channels    = options.audio_channels;

        char desc[512];
        int n = snprintf(desc, sizeof(desc),
            "audioconvert ! audioresample ! appsink name=avb_asink sync=false "
            "max-buffers=0 caps=audio/x-raw,format=F32LE,layout=interleaved");
        if (m_req_channels > 0)
            n += snprintf(desc + n, sizeof(desc) - n, ",channels=%d", m_req_channels);
        if (m_req_sample_rate > 0)
            n += snprintf(desc + n, sizeof(desc) - n, ",rate=%d", m_req_sample_rate);

        GstElement *abin = m_gst.gst_parse_bin_from_description(desc, TRUE, &err);
        if (!abin) {
            set_error("Failed to build audio sink bin: %s",
                      err && err->message ? err->message : "unknown");
            m_gst.g_clear_error(&err);
            m_gst.g_free(uri);
            return AVB_ERROR_OPEN_FAILED;
        }
        m_gst.g_object_set(m_pipeline, "audio-sink", abin, nullptr);
        m_audio_sink = m_gst.gst_bin_get_by_name((GstBin *)abin, "avb_asink");
        if (m_audio_sink) m_gst.gst_app_sink_set_drop((GstAppSink *)m_audio_sink, FALSE);
        flags |= AVB_GST_PLAY_FLAG_AUDIO;
    }

    // Build the video sink bin: convert to the requested packed/planar format.
    if (options.enable_video) {
        // Bound the decoded-video buffering so that a consumer pulling at
        // playback rate (slower than the decoder) does not let raw frames pile
        // up without limit — for 1080p RGBA that is ~8 MB/frame and reaches
        // gigabytes within seconds. max-buffers applies backpressure: the video
        // branch blocks its streaming thread once the sink is full, pausing the
        // decoder until the consumer pulls.
        //
        // The audio sink is left unbounded (audio data is tiny), so the deadlock
        // that a *bounded* video sink could cause — a consumer draining audio
        // far ahead of video, backing up the demuxer and starving audio — is
        // avoided: audio always keeps flowing, and a normal player pulls both
        // streams roughly in step, well within this bound.
        char desc[256];
        snprintf(desc, sizeof(desc),
            "videoconvert ! appsink name=avb_vsink sync=false max-buffers=16 "
            "caps=video/x-raw,format=%s", vfmt);

        GstElement *vbin = m_gst.gst_parse_bin_from_description(desc, TRUE, &err);
        if (!vbin) {
            set_error("Failed to build video sink bin: %s",
                      err && err->message ? err->message : "unknown");
            m_gst.g_clear_error(&err);
            m_gst.g_free(uri);
            return AVB_ERROR_OPEN_FAILED;
        }
        m_gst.g_object_set(m_pipeline, "video-sink", vbin, nullptr);
        m_video_sink = m_gst.gst_bin_get_by_name((GstBin *)vbin, "avb_vsink");
        if (m_video_sink) m_gst.gst_app_sink_set_drop((GstAppSink *)m_video_sink, FALSE);
        flags |= AVB_GST_PLAY_FLAG_VIDEO;
    }

    m_gst.g_object_set(m_pipeline, "flags", flags, nullptr);

    // Preroll: move to PAUSED and wait for the state change to settle so that
    // negotiated caps and duration are available.
    m_gst.gst_element_set_state(m_pipeline, GST_STATE_PAUSED);
    GstState st_cur, st_pend;
    GstStateChangeReturn sret =
        m_gst.gst_element_get_state(m_pipeline, &st_cur, &st_pend, 10 * GST_SECOND);
    if (sret == GST_STATE_CHANGE_FAILURE) {
        set_error("Pipeline failed to reach PAUSED (unsupported/corrupt file?).");
        m_gst.g_free(uri);
        return AVB_ERROR_OPEN_FAILED;
    }

    // Duration (nanoseconds -> seconds).
    gint64 dur_ns = 0;
    if (m_gst.gst_element_query_duration(m_pipeline, GST_FORMAT_TIME, &dur_ns) && dur_ns > 0)
        m_duration = (double)dur_ns / GST_SECOND;

    // Read negotiated caps from each sink's preroll sample. A requested stream
    // that the file does not actually contain never prerolls; the timed pull
    // returns null and we drop that sink.
    if (m_audio_sink) {
        GstSample *s = m_gst.gst_app_sink_try_pull_preroll(
            (GstAppSink *)m_audio_sink, 3 * GST_SECOND);
        if (s) {
            GstCaps *caps = m_gst.gst_sample_get_caps(s);
            if (caps) {
                GstStructure *str = m_gst.gst_caps_get_structure(caps, 0);
                m_gst.gst_structure_get_int(str, "rate", &m_out_sample_rate);
                m_gst.gst_structure_get_int(str, "channels", &m_out_channels);
            }
            m_gst.gst_mini_object_unref((GstMiniObject *)s);
        } else {
            m_gst.gst_object_unref(m_audio_sink);
            m_audio_sink = nullptr;
        }
    }

    if (m_video_sink) {
        GstSample *s = m_gst.gst_app_sink_try_pull_preroll(
            (GstAppSink *)m_video_sink, 3 * GST_SECOND);
        if (s) {
            GstCaps *caps = m_gst.gst_sample_get_caps(s);
            if (caps) {
                GstStructure *str = m_gst.gst_caps_get_structure(caps, 0);
                m_gst.gst_structure_get_int(str, "width", &m_width);
                m_gst.gst_structure_get_int(str, "height", &m_height);
                int fn = 0, fd = 0;
                if (m_gst.gst_structure_get_fraction(str, "framerate", &fn, &fd) && fd != 0)
                    m_frame_rate = (double)fn / fd;
            }
            m_gst.gst_mini_object_unref((GstMiniObject *)s);
        } else {
            m_gst.gst_object_unref(m_video_sink);
            m_video_sink = nullptr;
        }
    }

    if (!m_audio_sink && !m_video_sink) {
        set_error("No supported audio or video stream found.");
        m_gst.g_free(uri);
        return AVB_ERROR_STREAM_NOT_FOUND;
    }

    discover_codec_names(uri);
    m_gst.g_free(uri);

    // Start decoding.
    m_gst.gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    return AVB_OK;
}

avb_result AvbBackendGStreamer::get_media_info(avb_media_info &out_info) {
    if (!m_pipeline) return AVB_ERROR_INVALID_ARGUMENT;

    out_info = {};
    out_info.backend_name = "gstreamer";
    out_info.duration_sec = m_duration;

    if (m_audio_sink) {
        out_info.audio.available    = 1;
        out_info.audio.stream_index = 0;
        out_info.audio.sample_rate  = m_out_sample_rate;
        out_info.audio.channels     = m_out_channels;
        out_info.audio.duration_sec = m_duration;
        out_info.audio.codec_name   = m_audio_codec_name.c_str();
    }
    if (m_video_sink) {
        out_info.video.available    = 1;
        out_info.video.stream_index = 0;
        out_info.video.width        = m_width;
        out_info.video.height       = m_height;
        out_info.video.frame_rate   = m_frame_rate;
        out_info.video.duration_sec = m_duration;
        out_info.video.codec_name   = m_video_codec_name.c_str();
    }
    return AVB_OK;
}

avb_result AvbBackendGStreamer::seek(double seconds) {
    if (!m_pipeline) return AVB_ERROR_INVALID_ARGUMENT;

    gint64 pos = (gint64)(seconds * GST_SECOND);
    gboolean ok = m_gst.gst_element_seek_simple(
        m_pipeline, GST_FORMAT_TIME,
        (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE), pos);
    if (!ok) {
        set_error("seek_simple failed.");
        return AVB_ERROR_SEEK_FAILED;
    }

    m_audio_buf.clear();
    m_audio_buf_pos = 0;
    m_audio_eof = false;
    m_seek_target = seconds;
    return AVB_OK;
}

bool AvbBackendGStreamer::fill_audio_buffer() {
    if (!m_audio_sink || m_audio_eof) return false;

    // pull_sample returns NULL at EOF (or if the sink is shut down).
    GstSample *sample = m_gst.gst_app_sink_pull_sample((GstAppSink *)m_audio_sink);
    if (!sample) {
        m_audio_eof = true;
        return false;
    }

    GstBuffer *buf = m_gst.gst_sample_get_buffer(sample);
    GstMapInfo map;
    memset(&map, 0, sizeof(map));
    if (buf && m_gst.gst_buffer_map(buf, &map, GST_MAP_READ)) {
        size_t n_floats = map.size / sizeof(float);
        const float *src = (const float *)map.data;
        m_audio_buf.insert(m_audio_buf.end(), src, src + n_floats);
        m_gst.gst_buffer_unmap(buf, &map);
    }
    m_gst.gst_mini_object_unref((GstMiniObject *)sample);
    return true;
}

int AvbBackendGStreamer::read_audio_f32(float *dst_interleaved, int frames) {
    if (!m_audio_sink || m_out_channels <= 0) return 0;

    int nb_channels     = m_out_channels;
    int samples_needed  = frames * nb_channels;
    int samples_written = 0;

    while (samples_written < samples_needed) {
        int available = (int)m_audio_buf.size() - m_audio_buf_pos;
        if (available > 0) {
            int to_copy = samples_needed - samples_written;
            if (to_copy > available) to_copy = available;
            memcpy(dst_interleaved + samples_written,
                   m_audio_buf.data() + m_audio_buf_pos,
                   to_copy * sizeof(float));
            m_audio_buf_pos += to_copy;
            samples_written += to_copy;
            if (m_audio_buf_pos >= (int)m_audio_buf.size()) {
                m_audio_buf.clear();
                m_audio_buf_pos = 0;
            }
            continue;
        }
        if (!fill_audio_buffer()) break;
    }

    return samples_written / nb_channels;
}

avb_result AvbBackendGStreamer::read_video_frame(avb_video_frame &out_frame) {
    if (!m_video_sink) return AVB_ERROR_STREAM_NOT_FOUND;

    while (true) {
        // pull_sample returns NULL at EOF (or if the sink is shut down).
        GstSample *sample = m_gst.gst_app_sink_pull_sample((GstAppSink *)m_video_sink);
        if (!sample) return AVB_ERROR_EOF;

        GstBuffer *buf  = m_gst.gst_sample_get_buffer(sample);
        GstCaps   *caps = m_gst.gst_sample_get_caps(sample);
        if (!buf || !caps) {
            m_gst.gst_mini_object_unref((GstMiniObject *)sample);
            return AVB_ERROR_DECODE_FAILED;
        }

        int w = m_width, h = m_height;
        GstStructure *str = m_gst.gst_caps_get_structure(caps, 0);
        m_gst.gst_structure_get_int(str, "width", &w);
        m_gst.gst_structure_get_int(str, "height", &h);

        // PTS (GstBuffer field is public). Drop pre-roll frames after a seek so
        // the first returned frame starts at ~seek target, matching the other
        // backends.
        double pts_sec = 0.0;
        if (GST_BUFFER_PTS_IS_VALID(buf))
            pts_sec = (double)GST_BUFFER_PTS(buf) / GST_SECOND;
        if (m_seek_target >= 0.0 && pts_sec < m_seek_target - 1e-3) {
            m_gst.gst_mini_object_unref((GstMiniObject *)sample);
            continue;
        }
        m_seek_target = -1.0;

        GstMapInfo map;
        memset(&map, 0, sizeof(map));
        if (!m_gst.gst_buffer_map(buf, &map, GST_MAP_READ)) {
            m_gst.gst_mini_object_unref((GstMiniObject *)sample);
            return AVB_ERROR_DECODE_FAILED;
        }

        // GStreamer raw video uses GST_ROUND_UP_4 plane strides when no
        // GstVideoMeta is attached. (Non-default strides via GstVideoMeta are a
        // known limitation.) Lay out planes per output format:
        //   RGBA/BGRA: 1 plane; NV12: 2 (Y, CbCr); I420: 3 (Y, Cb, Cr).
        int    plane_count = 1;
        int    stride[AVB_MAX_PLANES] = {0, 0, 0};
        int    rows[AVB_MAX_PLANES]   = {0, 0, 0};
        switch (m_video_format) {
            case AVB_PIXEL_FORMAT_NV12:
                plane_count = 2;
                stride[0] = round_up_4(w);     rows[0] = h;
                stride[1] = round_up_4(w);     rows[1] = h / 2;
                break;
            case AVB_PIXEL_FORMAT_I420:
                plane_count = 3;
                stride[0] = round_up_4(w);     rows[0] = h;
                stride[1] = round_up_4(w / 2); rows[1] = h / 2;
                stride[2] = round_up_4(w / 2); rows[2] = h / 2;
                break;
            default:
                plane_count = 1;
                stride[0] = round_up_4(w * 4); rows[0] = h;
                break;
        }

        size_t off[AVB_MAX_PLANES] = {0, 0, 0};
        size_t total = 0;
        for (int p = 0; p < plane_count; ++p) { off[p] = total; total += (size_t)stride[p] * rows[p]; }
        if (total > map.size) total = map.size; // never read past the buffer

        m_video_out_buf.resize(total);
        memcpy(m_video_out_buf.data(), map.data, total);

        out_frame = {};
        out_frame.width       = w;
        out_frame.height      = h;
        out_frame.format      = m_video_format;
        out_frame.pts_sec     = pts_sec;
        out_frame.plane_count = plane_count;
        for (int p = 0; p < plane_count; ++p) {
            out_frame.plane_data[p]   = m_video_out_buf.data() + off[p];
            out_frame.plane_stride[p] = stride[p];
        }
        out_frame.data      = out_frame.plane_data[0];
        out_frame.stride    = out_frame.plane_stride[0];
        out_frame.data_size = (int)m_video_out_buf.size();

        m_gst.gst_buffer_unmap(buf, &map);
        m_gst.gst_mini_object_unref((GstMiniObject *)sample);
        return AVB_OK;
    }
}

void AvbBackendGStreamer::release_video_frame(avb_video_frame &frame) {
    // Output buffer is backend-owned and reused on the next read; just zero the
    // caller's struct so stale pointers can't be used by accident.
    memset(&frame, 0, sizeof(frame));
}
