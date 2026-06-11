#include "avb_decoder_gstreamer.hpp"
#include "../../avb_video_codec_registry.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// playbin "flags" bits (from playbin's GstPlayFlags; values are stable ABI).
// Defined locally so we don't depend on the flags enum being in public headers.
enum {
    AVB_GST_PLAY_FLAG_VIDEO = (1 << 0),
    AVB_GST_PLAY_FLAG_AUDIO = (1 << 1),
};

static inline int round_up_4(int x) { return (x + 3) & ~3; }

static constexpr uint32_t avb_drm_fourcc(char a, char b, char c, char d) {
    return (uint32_t)(uint8_t)a |
           ((uint32_t)(uint8_t)b << 8) |
           ((uint32_t)(uint8_t)c << 16) |
           ((uint32_t)(uint8_t)d << 24);
}

static gboolean avb_gst_dmabuf_propose_allocation(
    GstAppSink *,
    GstQuery *query,
    gpointer user_data
) {
    auto *gst = static_cast<AvbGstFuncs *>(user_data);
    if (!gst || !gst->gst_query_add_allocation_meta ||
        !gst->gst_video_meta_api_get_type) {
        return FALSE;
    }
    gst->gst_query_add_allocation_meta(
        query, gst->gst_video_meta_api_get_type(), nullptr);
    return TRUE;
}

static bool avb_is_compressed_format(avb_pixel_format fmt) {
    return fmt == AVB_PIXEL_FORMAT_BC1_RGBA ||
           fmt == AVB_PIXEL_FORMAT_BC3_RGBA ||
           fmt == AVB_PIXEL_FORMAT_BC4_R ||
           fmt == AVB_PIXEL_FORMAT_BC5_RG ||
           fmt == AVB_PIXEL_FORMAT_BC7_RGBA;
}

static std::string gst_launch_quote(const char *s) {
    std::string out = "\"";
    for (const char *p = s; p && *p; ++p) {
        if (*p == '\\' || *p == '"') out.push_back('\\');
        out.push_back(*p);
    }
    out.push_back('"');
    return out;
}

static bool parse_gst_drm_format(const char *text, uint32_t &fourcc, uint64_t &modifier) {
    if (!text || strlen(text) < 4) return false;
    fourcc = avb_drm_fourcc(text[0], text[1], text[2], text[3]);
    modifier = UINT64_MAX;
    const char *colon = strchr(text, ':');
    if (colon && colon[1]) {
        char *end = nullptr;
        modifier = (uint64_t)strtoull(colon + 1, &end, 0);
        if (end == colon + 1) modifier = UINT64_MAX;
    }
    return true;
}

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

AvbDecoderGStreamer::AvbDecoderGStreamer() {
    char err_buf[512];
    m_libs_loaded = avb_gst_load(m_gst, err_buf, sizeof(err_buf));
    if (!m_libs_loaded) {
        m_last_error = err_buf;
        return;
    }
    m_gst.gst_init(nullptr, nullptr);
}

AvbDecoderGStreamer::~AvbDecoderGStreamer() {
    close_internal();
}

const char *AvbDecoderGStreamer::get_backend_name() const { return "gstreamer"; }
const char *AvbDecoderGStreamer::get_last_error() const {
    return m_last_error.empty() ? nullptr : m_last_error.c_str();
}

void AvbDecoderGStreamer::set_error(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    m_last_error = buf;
}

void AvbDecoderGStreamer::close_internal() {
    if (!m_libs_loaded) return;

    if (m_video_preroll_sample) {
        m_gst.gst_mini_object_unref((GstMiniObject *)m_video_preroll_sample);
        m_video_preroll_sample = nullptr;
    }
    if (m_native_video_sample) {
        m_gst.gst_mini_object_unref((GstMiniObject *)m_native_video_sample);
        m_native_video_sample = nullptr;
    }
    if (m_custom_video_decoder) {
        if (m_custom_video_decoder->close && m_custom_video_ctx)
            m_custom_video_decoder->close(m_custom_video_ctx);
        m_custom_video_decoder = nullptr;
        m_custom_video_ctx = nullptr;
    }
    if (m_audio_sink) { m_gst.gst_object_unref(m_audio_sink); m_audio_sink = nullptr; }
    if (m_video_sink) { m_gst.gst_object_unref(m_video_sink); m_video_sink = nullptr; }
    if (m_pipeline) {
        m_gst.gst_element_set_state(m_pipeline, GST_STATE_NULL);
        m_gst.gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
    }

    m_audio_buf.clear();
    m_audio_buf_pos = 0;
    m_audio_buf_pts = -1.0;
    m_video_out_buf.clear();
    m_out_sample_rate = 0;
    m_out_channels    = 0;
    m_audio_track       = 0;
    m_audio_track_count = 0;
    m_width = m_height = 0;
    m_frame_rate = 0.0;
    m_duration   = 0.0;
    m_audio_eof  = false;
    m_seek_target = -1.0;
    m_custom_pipeline = false;
    m_audio_codec_name.clear();
    m_video_codec_name.clear();
    m_video_memory = AVB_VIDEO_MEMORY_CPU;
    m_hw_device = AVB_HW_DEVICE_AUTO;
}

void AvbDecoderGStreamer::discover_codec_names(const char *uri) {
    GError *err = nullptr;
    GstDiscoverer *disc = m_gst.gst_discoverer_new(5 * GST_SECOND, &err);
    if (!disc) { m_gst.g_clear_error(&err); return; }

    GstDiscovererInfo *info = m_gst.gst_discoverer_discover_uri(disc, uri, &err);
    if (info) {
        GList *al = m_gst.gst_discoverer_info_get_audio_streams(info);
        if (al) {
            // Report the *selected* track's codec (m_audio_track), not just the
            // first audio stream.
            GList *node = al;
            for (int i = 0; i < m_audio_track && node->next; ++i) node = node->next;
            GstCaps *caps = m_gst.gst_discoverer_stream_info_get_caps(
                (GstDiscovererStreamInfo *)node->data);
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

avb_result AvbDecoderGStreamer::open_custom_file(
    const char *path,
    const avb_decode_options &options
) {
    if (!options.enable_video || !options.enable_custom_video_decoders ||
        !avb_is_compressed_format(options.video_format)) {
        return AVB_ERROR_STREAM_NOT_FOUND;
    }

    std::string desc = "filesrc location=" + gst_launch_quote(path) +
        " ! qtdemux name=demux "
        "demux.video_0 ! queue ! appsink name=avb_vsink sync=false max-buffers=16";

    if (options.enable_audio) {
        desc += " demux.audio_0 ! queue ! decodebin ! audioconvert ! audioresample ! "
                "appsink name=avb_asink sync=false max-buffers=0 "
                "caps=audio/x-raw,format=F32LE,layout=interleaved";
        if (options.audio_channels > 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), ",channels=%d", options.audio_channels);
            desc += buf;
        }
        if (options.audio_sample_rate > 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), ",rate=%d", options.audio_sample_rate);
            desc += buf;
        }
    }

    GError *err = nullptr;
    m_pipeline = m_gst.gst_parse_launch(desc.c_str(), &err);
    if (!m_pipeline) {
        set_error("Failed to build custom GStreamer pipeline: %s",
                  err && err->message ? err->message : "unknown");
        m_gst.g_clear_error(&err);
        return AVB_ERROR_OPEN_FAILED;
    }

    m_video_sink = m_gst.gst_bin_get_by_name((GstBin *)m_pipeline, "avb_vsink");
    if (m_video_sink) m_gst.gst_app_sink_set_drop((GstAppSink *)m_video_sink, FALSE);
    if (options.enable_audio) {
        m_audio_sink = m_gst.gst_bin_get_by_name((GstBin *)m_pipeline, "avb_asink");
        if (m_audio_sink) m_gst.gst_app_sink_set_drop((GstAppSink *)m_audio_sink, FALSE);
    }
    if (!m_video_sink) {
        set_error("Custom GStreamer pipeline did not create a video appsink.");
        return AVB_ERROR_OPEN_FAILED;
    }

    m_gst.gst_element_set_state(m_pipeline, GST_STATE_PAUSED);
    GstState st_cur, st_pend;
    GstStateChangeReturn sret =
        m_gst.gst_element_get_state(m_pipeline, &st_cur, &st_pend, 10 * GST_SECOND);
    if (sret == GST_STATE_CHANGE_FAILURE) {
        set_error("Custom GStreamer pipeline failed to reach PAUSED.");
        return AVB_ERROR_STREAM_NOT_FOUND;
    }

    gint64 dur_ns = 0;
    if (m_gst.gst_element_query_duration(m_pipeline, GST_FORMAT_TIME, &dur_ns) && dur_ns > 0)
        m_duration = (double)dur_ns / GST_SECOND;

    m_video_preroll_sample = m_gst.gst_app_sink_try_pull_preroll(
        (GstAppSink *)m_video_sink, 3 * GST_SECOND);
    if (!m_video_preroll_sample) {
        set_error("Custom GStreamer pipeline did not preroll a video packet.");
        return AVB_ERROR_STREAM_NOT_FOUND;
    }

    GstCaps *vcaps = m_gst.gst_sample_get_caps(m_video_preroll_sample);
    avb_video_stream_info stream{};
    stream.stream_index = 0;
    stream.duration_sec = m_duration;
    stream.time_base_num = 1;
    stream.time_base_den = (int)GST_SECOND;
    m_video_codec_name = caps_to_codec_name(m_gst, vcaps);
    stream.codec_name = m_video_codec_name.empty() ? nullptr : m_video_codec_name.c_str();
    if (vcaps) {
        GstStructure *str = m_gst.gst_caps_get_structure(vcaps, 0);
        if (str) {
            m_gst.gst_structure_get_int(str, "width", &m_width);
            m_gst.gst_structure_get_int(str, "height", &m_height);
            int fn = 0, fd = 0;
            if (m_gst.gst_structure_get_fraction(str, "framerate", &fn, &fd) && fd != 0)
                m_frame_rate = (double)fn / fd;
        }
    }
    stream.width = m_width;
    stream.height = m_height;
    stream.frame_rate = m_frame_rate;

    const avb_video_decoder_plugin *plugin =
        avb_find_video_decoder_plugin(stream, options);
    if (!plugin) return AVB_ERROR_STREAM_NOT_FOUND;

    void *ctx = nullptr;
    avb_result res = plugin->open(&ctx, &stream, &options);
    if (res != AVB_OK) {
        set_error("Custom video decoder '%s' failed to open.",
                  plugin->name ? plugin->name : "(unnamed)");
        return res;
    }
    m_custom_video_decoder = plugin;
    m_custom_video_ctx = ctx;
    m_custom_pipeline = true;
    m_video_format = options.video_format;

    if (m_audio_sink) {
        GstSample *as = m_gst.gst_app_sink_try_pull_preroll(
            (GstAppSink *)m_audio_sink, 3 * GST_SECOND);
        if (as) {
            GstCaps *acaps = m_gst.gst_sample_get_caps(as);
            if (acaps) {
                GstStructure *str = m_gst.gst_caps_get_structure(acaps, 0);
                if (str) {
                    m_gst.gst_structure_get_int(str, "rate", &m_out_sample_rate);
                    m_gst.gst_structure_get_int(str, "channels", &m_out_channels);
                }
            }
            m_gst.gst_mini_object_unref((GstMiniObject *)as);
        } else {
            m_gst.gst_object_unref(m_audio_sink);
            m_audio_sink = nullptr;
        }
        m_req_sample_rate = options.audio_sample_rate;
        m_req_channels = options.audio_channels;
        m_audio_track = 0;
        m_audio_track_count = m_audio_sink ? 1 : 0;
    }

    GError *uri_err = nullptr;
    gchar *uri = m_gst.gst_filename_to_uri(path, &uri_err);
    if (uri) {
        std::string custom_video_codec = m_video_codec_name;
        discover_codec_names(uri);
        m_video_codec_name = custom_video_codec;
        m_gst.g_free(uri);
    } else {
        m_gst.g_clear_error(&uri_err);
    }

    m_gst.gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    return AVB_OK;
}

avb_result AvbDecoderGStreamer::open_file(const char *path, const avb_decode_options &options) {
    if (!m_libs_loaded) return AVB_ERROR_BACKEND_NOT_AVAILABLE;

    close_internal();

    if (!options.enable_audio && !options.enable_video) {
        set_error("Neither audio nor video requested.");
        return AVB_ERROR_INVALID_ARGUMENT;
    }

    if (options.enable_video && options.enable_custom_video_decoders &&
        avb_is_compressed_format(options.video_format)) {
        avb_result custom_res = open_custom_file(path, options);
        if (custom_res == AVB_OK) return AVB_OK;
        if (custom_res != AVB_ERROR_STREAM_NOT_FOUND) return custom_res;
        close_internal();
    }

    m_video_memory = options.video_memory;
    m_hw_device = options.hardware_device == AVB_HW_DEVICE_AUTO
        ? AVB_HW_DEVICE_VAAPI : options.hardware_device;
    if (m_video_memory == AVB_VIDEO_MEMORY_NATIVE &&
        options.hardware_policy == AVB_HARDWARE_DISABLED) {
        set_error("Native video output requires hardware_policy PREFER or REQUIRE.");
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    if (m_video_memory == AVB_VIDEO_MEMORY_DMABUF &&
        options.hardware_policy == AVB_HARDWARE_DISABLED) {
        m_hw_device = AVB_HW_DEVICE_VAAPI;
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
                                      m_video_format = AVB_PIXEL_FORMAT_BGRA8; vfmt = "BGRA"; break;
        default:
            set_error("Requested video pixel format is not supported by GStreamer conversion.");
            m_gst.g_free(uri);
            return AVB_ERROR_INVALID_ARGUMENT;
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
        // Bound the decoded-video buffering. appsink queues *decoded* frames, so
        // an unbounded sink lets memory pile up without limit when the consumer
        // pulls slower than the decoder — for 1080p RGBA that is ~8 MB/frame and
        // reaches gigabytes within seconds. max-buffers applies backpressure: the
        // video branch blocks its streaming thread once the sink is full, pausing
        // the decoder until the consumer pulls. A normal player pulls both
        // streams in step, so it never fills.
        //
        // Consequence (documented on avb_decoder_read_*): the two streams must be
        // read roughly interleaved. Draining one to EOF while never pulling the
        // other deadlocks — the full video sink blocks its streaming thread,
        // backs up the demuxer and starves audio. For bulk per-stream decoding,
        // open a decoder with only that stream enabled.
        char desc[256];
        if (m_video_memory == AVB_VIDEO_MEMORY_NATIVE) {
            snprintf(desc, sizeof(desc),
                "appsink name=avb_vsink sync=false max-buffers=16 "
                "caps=video/x-raw(memory:VASurface)");
        } else if (m_video_memory == AVB_VIDEO_MEMORY_DMABUF) {
            snprintf(desc, sizeof(desc),
                "appsink name=avb_vsink sync=false max-buffers=16 "
                "caps=video/x-raw(memory:DMABuf)");
        } else {
            snprintf(desc, sizeof(desc),
                "videoconvert ! appsink name=avb_vsink sync=false max-buffers=16 "
                "caps=video/x-raw,format=%s", vfmt);
        }

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
        if (m_video_sink) {
            m_gst.gst_app_sink_set_drop((GstAppSink *)m_video_sink, FALSE);
            if (m_video_memory == AVB_VIDEO_MEMORY_DMABUF) {
                GstAppSinkCallbacks callbacks{};
                callbacks.propose_allocation = avb_gst_dmabuf_propose_allocation;
                m_gst.gst_app_sink_set_callbacks(
                    (GstAppSink *)m_video_sink, &callbacks, &m_gst, nullptr);
            }
        }
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

    // Start decoding.
    m_gst.gst_element_set_state(m_pipeline, GST_STATE_PLAYING);

    // Audio track selection. playbin lists the tracks it found in "n-audio" and
    // selects one by logical 0-based "current-audio". We switch here (in PLAYING)
    // and flush via a seek to 0 to drop stale buffers from the previous track —
    // a flushing seek while PAUSED can wedge the pipeline, but the PLAYING seek
    // path is the same one used for normal seeking. Priming fill_audio_buffer()
    // then reads the selected track's real channels/rate from a live sample.
    if (m_audio_sink) {
        gint n_audio = 0, current = 0;
        m_gst.g_object_get(m_pipeline, "n-audio", &n_audio, nullptr);
        m_audio_track_count = n_audio;

        int want = options.audio_stream_index < 0 ? 0 : options.audio_stream_index;
        if (n_audio > 0 && want >= n_audio) want = n_audio - 1;

        m_gst.g_object_get(m_pipeline, "current-audio", &current, nullptr);
        if (n_audio > 0 && want != current) {
            m_gst.g_object_set(m_pipeline, "current-audio", (gint)want, nullptr);
            m_gst.gst_element_seek_simple(
                m_pipeline, GST_FORMAT_TIME,
                (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE), 0);
        }
        m_gst.g_object_get(m_pipeline, "current-audio", &current, nullptr);
        m_audio_track = current < 0 ? 0 : current;

        fill_audio_buffer(); // prime channels/rate from the selected track
    }

    // Codec names (uses m_audio_track, set above).
    discover_codec_names(uri);
    m_gst.g_free(uri);

    return AVB_OK;
}

avb_result AvbDecoderGStreamer::get_media_info(avb_media_info &out_info) {
    if (!m_pipeline) return AVB_ERROR_INVALID_ARGUMENT;

    out_info = {};
    out_info.backend_name = "gstreamer";
    out_info.duration_sec = m_duration;

    if (m_audio_sink) {
        out_info.audio.available    = 1;
        out_info.audio.stream_index = m_audio_track;
        out_info.audio.track_count  = m_audio_track_count > 0 ? m_audio_track_count : 1;
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

avb_result AvbDecoderGStreamer::seek(double seconds) {
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
    m_audio_buf_pts = -1.0;
    m_audio_eof = false;
    m_seek_target = seconds;
    if (m_custom_video_decoder && m_custom_video_decoder->flush)
        m_custom_video_decoder->flush(m_custom_video_ctx);
    if (m_video_preroll_sample) {
        m_gst.gst_mini_object_unref((GstMiniObject *)m_video_preroll_sample);
        m_video_preroll_sample = nullptr;
    }
    return AVB_OK;
}

bool AvbDecoderGStreamer::fill_audio_buffer() {
    if (!m_audio_sink || m_audio_eof) return false;

    // pull_sample returns NULL at EOF (or if the sink is shut down).
    GstSample *sample = m_gst.gst_app_sink_pull_sample((GstAppSink *)m_audio_sink);
    if (!sample) {
        m_audio_eof = true;
        return false;
    }

    // Track the negotiated output format from the live sample, so it reflects
    // the actually-selected audio track (the preroll caps can be stale after a
    // track switch).
    GstCaps *scaps = m_gst.gst_sample_get_caps(sample);
    if (scaps) {
        GstStructure *str = m_gst.gst_caps_get_structure(scaps, 0);
        if (str) {
            m_gst.gst_structure_get_int(str, "rate", &m_out_sample_rate);
            m_gst.gst_structure_get_int(str, "channels", &m_out_channels);
        }
    }

    GstBuffer *buf = m_gst.gst_sample_get_buffer(sample);
    GstMapInfo map;
    memset(&map, 0, sizeof(map));
    if (buf && m_gst.gst_buffer_map(buf, &map, GST_MAP_READ)) {
        size_t n_floats = map.size / sizeof(float);
        const float *src = (const float *)map.data;
        // If this buffer becomes the head of an empty queue, record its PTS.
        if (m_audio_buf_pos >= (int)m_audio_buf.size()) {
            m_audio_buf.clear();
            m_audio_buf_pos = 0;
            m_audio_buf_pts = GST_BUFFER_PTS_IS_VALID(buf)
                ? (double)GST_BUFFER_PTS(buf) / GST_SECOND : -1.0;
        }
        m_audio_buf.insert(m_audio_buf.end(), src, src + n_floats);
        m_gst.gst_buffer_unmap(buf, &map);
    }
    m_gst.gst_mini_object_unref((GstMiniObject *)sample);
    return true;
}

int AvbDecoderGStreamer::read_audio_f32(float *dst_interleaved, int frames) {
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
            if (m_audio_buf_pts >= 0.0)
                m_audio_buf_pts += (double)(to_copy / nb_channels) / m_out_sample_rate;
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

double AvbDecoderGStreamer::audio_next_pts() {
    if (!m_audio_sink || m_out_channels <= 0) return -1.0;
    if (m_audio_buf_pos >= (int)m_audio_buf.size()) {
        if (!fill_audio_buffer()) return -1.0;
    }
    return m_audio_buf_pts;
}

avb_result AvbDecoderGStreamer::read_custom_video_frame(avb_video_frame &out_frame) {
    if (!m_custom_video_decoder || !m_custom_video_ctx || !m_video_sink)
        return AVB_ERROR_STREAM_NOT_FOUND;

    while (true) {
        GstSample *sample = m_video_preroll_sample;
        m_video_preroll_sample = nullptr;
        if (!sample)
            sample = m_gst.gst_app_sink_pull_sample((GstAppSink *)m_video_sink);
        if (!sample) return AVB_ERROR_EOF;

        GstBuffer *buf = m_gst.gst_sample_get_buffer(sample);
        if (!buf) {
            m_gst.gst_mini_object_unref((GstMiniObject *)sample);
            return AVB_ERROR_DECODE_FAILED;
        }

        double pts_sec = GST_BUFFER_PTS_IS_VALID(buf)
            ? (double)GST_BUFFER_PTS(buf) / GST_SECOND : -1.0;
        if (m_seek_target >= 0.0 && pts_sec >= 0.0 &&
            pts_sec < m_seek_target - 1e-3) {
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

        avb_encoded_packet packet{};
        packet.data = map.data;
        packet.size = (int)map.size;
        packet.pts_sec = pts_sec;
        packet.duration_sec = GST_BUFFER_DURATION_IS_VALID(buf)
            ? (double)GST_BUFFER_DURATION(buf) / GST_SECOND : -1.0;
        packet.keyframe = !GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT);
        packet.stream_index = 0;
        packet.pts = GST_BUFFER_PTS_IS_VALID(buf) ? (int64_t)GST_BUFFER_PTS(buf) : -1;
        packet.dts = GST_BUFFER_DTS_IS_VALID(buf) ? (int64_t)GST_BUFFER_DTS(buf) : -1;
        packet.duration = GST_BUFFER_DURATION_IS_VALID(buf) ? (int64_t)GST_BUFFER_DURATION(buf) : -1;
        packet.time_base_num = 1;
        packet.time_base_den = (int)GST_SECOND;

        avb_result res = m_custom_video_decoder->decode_packet(
            m_custom_video_ctx, &packet, &out_frame);
        m_gst.gst_buffer_unmap(buf, &map);
        m_gst.gst_mini_object_unref((GstMiniObject *)sample);

        if (res == AVB_OK) {
            if (out_frame.pts_sec < 0.0) out_frame.pts_sec = pts_sec;
            return AVB_OK;
        }
        if (res == AVB_ERROR_AGAIN) continue;
        return res;
    }
}

avb_result AvbDecoderGStreamer::fill_dmabuf_video_frame(
    GstSample *sample,
    GstBuffer *buf,
    GstCaps *caps,
    int w,
    int h,
    double pts_sec,
    avb_video_frame &out_frame
) {
    GstVideoMeta *meta = m_gst.gst_buffer_get_video_meta(buf);
    uint32_t drm_format = 0;
    uint64_t drm_modifier = UINT64_MAX;
    if (caps) {
        const GstStructure *s = m_gst.gst_caps_get_structure(caps, 0);
        if (s) {
            parse_gst_drm_format(
                m_gst.gst_structure_get_string(s, "drm-format"),
                drm_format, drm_modifier);
        }
    }
    guint memories = m_gst.gst_buffer_n_memory(buf);
    int want_planes = meta ? (int)meta->n_planes : (int)memories;
    if (want_planes <= 0) {
        set_error("GStreamer DMABUF frame has no planes.");
        return AVB_ERROR_DECODE_FAILED;
    }

    if (m_native_video_sample) {
        m_gst.gst_mini_object_unref((GstMiniObject *)m_native_video_sample);
        m_native_video_sample = nullptr;
    }

    out_frame = {};
    out_frame.width = w;
    out_frame.height = h;
    out_frame.format = AVB_PIXEL_FORMAT_UNKNOWN;
    out_frame.pts_sec = pts_sec;
    out_frame.memory_type = AVB_VIDEO_MEMORY_DMABUF;
    out_frame.hardware_device = m_hw_device;
    out_frame.native_handle = buf;
    out_frame.native_owner = this;
    out_frame.drm_format = drm_format;
    out_frame.native_handle_id = drm_format;
    for (int p = 0; p < AVB_MAX_PLANES; ++p) out_frame.dmabuf_fd[p] = -1;

    int planes = 0;
    for (int p = 0; p < want_planes && p < AVB_MAX_PLANES; ++p) {
        guint mem_index = memories == 1 ? 0 : (guint)p;
        GstMemory *mem = mem_index < memories
            ? m_gst.gst_buffer_peek_memory(buf, mem_index)
            : nullptr;
        if (!mem || !m_gst.gst_is_dmabuf_memory(mem)) {
            set_error("GStreamer video buffer memory is not DMABUF.");
            return AVB_ERROR_DECODE_FAILED;
        }
        out_frame.dmabuf_fd[p] = m_gst.gst_dmabuf_memory_get_fd(mem);
        if (meta) {
            out_frame.plane_offset[p] = (int)meta->offset[p];
            out_frame.plane_stride[p] = meta->stride[p];
        }
        out_frame.dmabuf_modifier[p] = drm_modifier;
        ++planes;
    }
    out_frame.plane_count = planes;
    out_frame.stride = planes > 0 ? out_frame.plane_stride[0] : 0;
    m_native_video_sample = sample;
    return AVB_OK;
}

avb_result AvbDecoderGStreamer::read_video_frame(avb_video_frame &out_frame) {
    if (m_custom_pipeline) return read_custom_video_frame(out_frame);
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

        if (m_video_memory == AVB_VIDEO_MEMORY_NATIVE) {
            if (m_native_video_sample) {
                m_gst.gst_mini_object_unref((GstMiniObject *)m_native_video_sample);
                m_native_video_sample = nullptr;
            }
            m_native_video_sample = sample;
            out_frame = {};
            out_frame.width = w;
            out_frame.height = h;
            out_frame.format = m_video_format;
            out_frame.pts_sec = pts_sec;
            out_frame.memory_type = AVB_VIDEO_MEMORY_NATIVE;
            out_frame.hardware_device = m_hw_device;
            out_frame.native_handle = buf;
            out_frame.native_owner = this;
            for (int p = 0; p < AVB_MAX_PLANES; ++p) out_frame.dmabuf_fd[p] = -1;
            return AVB_OK;
        }
        if (m_video_memory == AVB_VIDEO_MEMORY_DMABUF) {
            avb_result res = fill_dmabuf_video_frame(
                sample, buf, caps, w, h, pts_sec, out_frame);
            if (res != AVB_OK)
                m_gst.gst_mini_object_unref((GstMiniObject *)sample);
            return res;
        }

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
        out_frame.memory_type = AVB_VIDEO_MEMORY_CPU;
        out_frame.hardware_device = AVB_HW_DEVICE_AUTO;
        out_frame.plane_count = plane_count;
        for (int p = 0; p < AVB_MAX_PLANES; ++p) out_frame.dmabuf_fd[p] = -1;
        for (int p = 0; p < plane_count; ++p) {
            out_frame.plane_data[p]   = m_video_out_buf.data() + off[p];
            out_frame.plane_stride[p] = stride[p];
            out_frame.plane_offset[p] = (int)off[p];
        }
        out_frame.data      = out_frame.plane_data[0];
        out_frame.stride    = out_frame.plane_stride[0];
        out_frame.data_size = (int)m_video_out_buf.size();

        m_gst.gst_buffer_unmap(buf, &map);
        m_gst.gst_mini_object_unref((GstMiniObject *)sample);
        return AVB_OK;
    }
}

void AvbDecoderGStreamer::release_video_frame(avb_video_frame &frame) {
    if (m_custom_pipeline) {
        if (m_custom_video_decoder && m_custom_video_decoder->release_frame)
            m_custom_video_decoder->release_frame(m_custom_video_ctx, &frame);
        memset(&frame, 0, sizeof(frame));
        return;
    }
    if ((frame.memory_type == AVB_VIDEO_MEMORY_NATIVE ||
         frame.memory_type == AVB_VIDEO_MEMORY_DMABUF) &&
        frame.native_owner == this && m_native_video_sample) {
        m_gst.gst_mini_object_unref((GstMiniObject *)m_native_video_sample);
        m_native_video_sample = nullptr;
    }
    // Output buffer is backend-owned and reused on the next read; just zero the
    // caller's struct so stale pointers can't be used by accident.
    memset(&frame, 0, sizeof(frame));
}
