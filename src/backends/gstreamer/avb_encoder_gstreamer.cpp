#include "avb_encoder_gstreamer.hpp"
#include "../../avb_video_codec_registry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cinttypes>
#include <cstdint>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static inline int round_up_4(int x) { return (x + 3) & ~3; }

static constexpr uint32_t avb_drm_fourcc(char a, char b, char c, char d) {
    return (uint32_t)(uint8_t)a |
           ((uint32_t)(uint8_t)b << 8) |
           ((uint32_t)(uint8_t)c << 16) |
           ((uint32_t)(uint8_t)d << 24);
}

static bool avb_is_compressed_format(avb_pixel_format fmt) {
    return fmt == AVB_PIXEL_FORMAT_BC1_RGBA ||
           fmt == AVB_PIXEL_FORMAT_BC3_RGBA ||
           fmt == AVB_PIXEL_FORMAT_BC4_R ||
           fmt == AVB_PIXEL_FORMAT_BC5_RG ||
           fmt == AVB_PIXEL_FORMAT_BC7_RGBA;
}

static const char *gst_hw_video_encoder(avb_video_codec codec, avb_hardware_device device) {
    avb_video_codec c = codec == AVB_VIDEO_CODEC_AUTO ? AVB_VIDEO_CODEC_H264 : codec;
    if (device != AVB_HW_DEVICE_AUTO && device != AVB_HW_DEVICE_VAAPI)
        return nullptr;
    switch (c) {
        case AVB_VIDEO_CODEC_H264: return "vah264enc";
        case AVB_VIDEO_CODEC_HEVC: return "vah265enc";
        case AVB_VIDEO_CODEC_VP9:  return "vavp9lpenc";
        default: return nullptr;
    }
}

static size_t dmabuf_plane_size(const avb_video_frame &frame, int plane) {
    int rows = frame.height;
    if (frame.plane_count == 2 && plane == 1) rows = frame.height / 2;
    if (frame.plane_count == 3 && plane > 0) rows = frame.height / 2;
    if (rows <= 0 || frame.plane_stride[plane] <= 0) return 0;
    return (size_t)frame.plane_offset[plane] +
           (size_t)frame.plane_stride[plane] * (size_t)rows;
}

static size_t dmabuf_object_size(int fd, size_t fallback) {
    struct stat st {};
    if (fd >= 0 && fstat(fd, &st) == 0 && st.st_size > 0)
        return std::max((size_t)st.st_size, fallback);
    return fallback;
}

static bool drm_fourcc_to_string(uint32_t fourcc, char out[5]) {
    if (!fourcc) return false;
    out[0] = (char)(fourcc & 0xff);
    out[1] = (char)((fourcc >> 8) & 0xff);
    out[2] = (char)((fourcc >> 16) & 0xff);
    out[3] = (char)((fourcc >> 24) & 0xff);
    out[4] = '\0';
    for (int i = 0; i < 4; ++i) {
        unsigned char c = (unsigned char)out[i];
        if (c < 0x20 || c > 0x7e) return false;
    }
    return true;
}

static GstVideoFormat gst_video_format_from_drm_fourcc(uint32_t fourcc) {
    switch (fourcc) {
        case avb_drm_fourcc('N', 'V', '1', '2'): return GST_VIDEO_FORMAT_NV12;
        case avb_drm_fourcc('B', 'G', 'R', 'A'): return GST_VIDEO_FORMAT_BGRA;
        case avb_drm_fourcc('R', 'G', 'B', 'A'): return GST_VIDEO_FORMAT_RGBA;
        case avb_drm_fourcc('A', 'R', '2', '4'): return GST_VIDEO_FORMAT_ARGB;
        case avb_drm_fourcc('A', 'B', '2', '4'): return GST_VIDEO_FORMAT_ABGR;
        default: return GST_VIDEO_FORMAT_UNKNOWN;
    }
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
    m_hw_video = false;
    m_input_memory = AVB_VIDEO_MEMORY_CPU;
    m_dmabuf_caps_format = 0;
    m_dmabuf_caps_modifier = UINT64_MAX;
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

avb_result AvbEncoderGStreamer::update_dmabuf_caps(const avb_video_frame &frame) {
    if (!m_vsrc || frame.drm_format == 0) {
        m_last_error = "DMABUF frame is missing drm_format.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }

    uint64_t modifier = frame.plane_count > 0 ? frame.dmabuf_modifier[0] : UINT64_MAX;
    if (m_dmabuf_caps_format == frame.drm_format &&
        m_dmabuf_caps_modifier == modifier) {
        return AVB_OK;
    }

    char fourcc[5];
    if (!drm_fourcc_to_string(frame.drm_format, fourcc)) {
        m_last_error = "DMABUF frame has an unsupported drm_format.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }

    char caps_str[384];
    if (modifier == 0 || modifier == UINT64_MAX) {
        snprintf(caps_str, sizeof(caps_str),
                 "video/x-raw(memory:DMABuf),format=DMA_DRM,drm-format=(string)%s,"
                 "width=%d,height=%d,framerate=%d/1",
                 fourcc, m_width, m_height, m_fps_n);
    } else {
        snprintf(caps_str, sizeof(caps_str),
                 "video/x-raw(memory:DMABuf),format=DMA_DRM,drm-format=(string)%s:0x%016" PRIx64 ","
                 "width=%d,height=%d,framerate=%d/1",
                 fourcc, modifier, m_width, m_height, m_fps_n);
    }

    GstCaps *caps = m_gst.gst_caps_from_string(caps_str);
    if (!caps) {
        char err_msg[512];
        snprintf(err_msg, sizeof(err_msg),
                 "gst_caps_from_string (DMABUF video) failed: %s", caps_str);
        m_last_error = err_msg;
        return AVB_ERROR_ENCODE_FAILED;
    }
    m_gst.g_object_set(m_vsrc, "caps", caps, nullptr);
    m_gst.gst_mini_object_unref((GstMiniObject *)caps);
    m_dmabuf_caps_format = frame.drm_format;
    m_dmabuf_caps_modifier = modifier;
    return AVB_OK;
}

GstBuffer *AvbEncoderGStreamer::build_dmabuf_buffer(const avb_video_frame &frame) {
    GstBuffer *buf = m_gst.gst_buffer_new();
    if (!buf) {
        m_last_error = "gst_buffer_new (DMABUF video) failed.";
        return nullptr;
    }

    GstAllocator *allocator = m_gst.gst_dmabuf_allocator_new();
    if (!allocator) {
        m_gst.gst_mini_object_unref((GstMiniObject *)buf);
        m_last_error = "gst_dmabuf_allocator_new failed.";
        return nullptr;
    }

    for (int p = 0; p < frame.plane_count && p < AVB_MAX_PLANES; ++p) {
        if (frame.dmabuf_fd[p] < 0) {
            m_gst.gst_object_unref(allocator);
            m_gst.gst_mini_object_unref((GstMiniObject *)buf);
            m_last_error = "DMABUF frame has an invalid fd.";
            return nullptr;
        }
        bool already_appended = false;
        for (int prev = 0; prev < p; ++prev) {
            if (frame.dmabuf_fd[prev] == frame.dmabuf_fd[p]) {
                already_appended = true;
                break;
            }
        }
        if (already_appended) continue;

        int owned_fd = dup(frame.dmabuf_fd[p]);
        if (owned_fd < 0) {
            m_gst.gst_object_unref(allocator);
            m_gst.gst_mini_object_unref((GstMiniObject *)buf);
            m_last_error = "dup(DMABUF fd) failed.";
            return nullptr;
        }
        size_t size = dmabuf_object_size(frame.dmabuf_fd[p],
                                         dmabuf_plane_size(frame, p));
        GstMemory *mem = m_gst.gst_dmabuf_allocator_alloc(allocator, owned_fd, size);
        if (!mem) {
            close(owned_fd);
            m_gst.gst_object_unref(allocator);
            m_gst.gst_mini_object_unref((GstMiniObject *)buf);
            m_last_error = "gst_dmabuf_allocator_alloc failed.";
            return nullptr;
        }
        m_gst.gst_buffer_append_memory(buf, mem);
    }
    m_gst.gst_object_unref(allocator);

    GstVideoFormat video_format = gst_video_format_from_drm_fourcc(frame.drm_format);
    if (video_format != GST_VIDEO_FORMAT_UNKNOWN) {
        gsize offsets[GST_VIDEO_MAX_PLANES] = {};
        gint strides[GST_VIDEO_MAX_PLANES] = {};
        guint n_planes = (guint)std::min(frame.plane_count, AVB_MAX_PLANES);
        for (guint p = 0; p < n_planes; ++p) {
            offsets[p] = (gsize)frame.plane_offset[p];
            strides[p] = (gint)frame.plane_stride[p];
        }
        m_gst.gst_buffer_add_video_meta_full(
            buf, (GstVideoFrameFlags)0, video_format, frame.width, frame.height,
            n_planes, offsets, strides);
    }
    return buf;
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
    bool audio_mux_is_encoder = false;
    const char *mux = nullptr;
    if (ends_with(".webm")) {
        mux = "webmmux";
    } else if (ends_with(".mkv")) {
        mux = "matroskamux";
    } else if (ends_with(".ogg")) {
        mux = "oggmux";
    } else if (ends_with(".wav")) {
        mux = "wavenc";
        audio_mux_is_encoder = true;
    } else if (ends_with(".flac")) {
        mux = "flacenc";
        audio_mux_is_encoder = true;
    } else if (ends_with(".mp3")) {
        mux = "lamemp3enc";
        audio_mux_is_encoder = true;
    } else if (ends_with(".mov")) {
        mux = "qtmux";
    } else {
        mux = "mp4mux";
    }
    if (audio_mux_is_encoder && options.video.enable) {
        m_last_error = "The selected audio-only output container cannot mux video.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }

    // Video codec -> encoder element (with its bitrate property/units).
    char venc[128] = "x264enc speed-preset=veryfast";
    char vparse[128] = "h264parse config-interval=-1 !";
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
        m_input_memory = options.video.input_memory;
        int kbps = options.video.bitrate / 1000;
        switch (options.video.codec) {
            case AVB_VIDEO_CODEC_AUTO:
            case AVB_VIDEO_CODEC_H264:
                if (kbps > 0) snprintf(venc, sizeof(venc), "x264enc speed-preset=veryfast bitrate=%d", kbps);
                else          snprintf(venc, sizeof(venc), "x264enc speed-preset=veryfast");
                snprintf(vparse, sizeof(vparse), "h264parse config-interval=-1 !");
                break;
            case AVB_VIDEO_CODEC_HEVC:
                if (kbps > 0) snprintf(venc, sizeof(venc), "x265enc speed-preset=veryfast bitrate=%d", kbps);
                else          snprintf(venc, sizeof(venc), "x265enc speed-preset=veryfast");
                snprintf(vparse, sizeof(vparse), "h265parse config-interval=-1 !");
                break;
            case AVB_VIDEO_CODEC_VP8:
                if (options.video.bitrate > 0) snprintf(venc, sizeof(venc), "vp8enc deadline=1 target-bitrate=%d", options.video.bitrate);
                else          snprintf(venc, sizeof(venc), "vp8enc deadline=1");
                vparse[0] = '\0';
                break;
            case AVB_VIDEO_CODEC_VP9:
                if (options.video.bitrate > 0) snprintf(venc, sizeof(venc), "vp9enc deadline=1 target-bitrate=%d", options.video.bitrate);
                else          snprintf(venc, sizeof(venc), "vp9enc deadline=1");
                vparse[0] = '\0';
                break;
            case AVB_VIDEO_CODEC_AV1:
                if (kbps > 0) snprintf(venc, sizeof(venc), "av1enc cpu-used=8 target-bitrate=%d", kbps);
                else          snprintf(venc, sizeof(venc), "av1enc cpu-used=8");
                snprintf(vparse, sizeof(vparse), "av1parse !");
                break;
            case AVB_VIDEO_CODEC_HAP:
                vparse[0] = '\0';
                break;
            default:
                m_last_error = "Invalid video codec (use AUTO/H264/HEVC/VP8/VP9/AV1/HAP).";
                return AVB_ERROR_INVALID_ARGUMENT;
        }
        if (options.video.hardware_policy != AVB_HARDWARE_DISABLED) {
            const char *hwenc = gst_hw_video_encoder(options.video.codec,
                                                     options.video.hardware_device);
            if (hwenc) {
                GstElement *probe = m_gst.gst_element_factory_make(hwenc, nullptr);
                if (probe) {
                    m_gst.gst_object_unref(probe);
                    if (kbps > 0) snprintf(venc, sizeof(venc), "%s bitrate=%d", hwenc, kbps);
                    else          snprintf(venc, sizeof(venc), "%s", hwenc);
                    m_hw_video = true;
                } else if (options.video.hardware_policy == AVB_HARDWARE_REQUIRE) {
                    m_last_error = "Required GStreamer hardware video encoder element is not available.";
                    return AVB_ERROR_INVALID_ARGUMENT;
                }
            } else if (options.video.hardware_policy == AVB_HARDWARE_REQUIRE) {
                m_last_error = "Required GStreamer hardware video encoder is not available for this codec/device.";
                return AVB_ERROR_INVALID_ARGUMENT;
            }
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
        custom_info.input_memory = options.video.input_memory;
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
            } else if (stream.codec == AVB_VIDEO_CODEC_HAP || options.video.codec == AVB_VIDEO_CODEC_HAP) {
                custom_vcaps = "video/x-hap,width=%d,height=%d,framerate=%d/1";
            } else {
                m_last_error = "Custom GStreamer video encoder requires gst_caps.";
                return AVB_ERROR_INVALID_ARGUMENT;
            }
        }
        if (!m_custom_video && options.video.codec == AVB_VIDEO_CODEC_HAP) {
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
    const char *audio_caps = "";
    bool audio_encoded_by_mux = false;
    if (options.audio.enable) {
        switch (options.audio.codec) {
            case AVB_AUDIO_CODEC_AUTO:
                audio_encoded_by_mux = audio_mux_is_encoder;
                if (!audio_encoded_by_mux) {
                    if (options.audio.bitrate > 0) snprintf(aenc, sizeof(aenc), "avenc_aac bitrate=%d", options.audio.bitrate);
                    else          snprintf(aenc, sizeof(aenc), "avenc_aac");
                } else if (ends_with(".wav")) {
                    audio_caps = "! audio/x-raw,format=S16LE";
                }
                break;
            case AVB_AUDIO_CODEC_AAC:
                if (audio_mux_is_encoder) {
                    m_last_error = "AAC requires a muxed container such as .mp4, .mov, or .mkv.";
                    return AVB_ERROR_INVALID_ARGUMENT;
                }
                if (options.audio.bitrate > 0) snprintf(aenc, sizeof(aenc), "avenc_aac bitrate=%d", options.audio.bitrate);
                else          snprintf(aenc, sizeof(aenc), "avenc_aac");
                break;
            case AVB_AUDIO_CODEC_OPUS:
                if (audio_mux_is_encoder) {
                    m_last_error = "Opus requires a muxed container such as .ogg, .webm, or .mkv.";
                    return AVB_ERROR_INVALID_ARGUMENT;
                }
                if (options.audio.bitrate > 0) snprintf(aenc, sizeof(aenc), "opusenc bitrate=%d", options.audio.bitrate);
                else          snprintf(aenc, sizeof(aenc), "opusenc");
                break;
            case AVB_AUDIO_CODEC_MP3:
                audio_encoded_by_mux = ends_with(".mp3");
                if (!audio_encoded_by_mux) {
                    if (options.audio.bitrate > 0) snprintf(aenc, sizeof(aenc), "lamemp3enc target=bitrate bitrate=%d", options.audio.bitrate / 1000);
                    else          snprintf(aenc, sizeof(aenc), "lamemp3enc");
                }
                break;
            case AVB_AUDIO_CODEC_FLAC:
                audio_encoded_by_mux = ends_with(".flac");
                if (!audio_encoded_by_mux) snprintf(aenc, sizeof(aenc), "flacenc");
                break;
            case AVB_AUDIO_CODEC_VORBIS:
                if (audio_mux_is_encoder) {
                    m_last_error = "Vorbis requires a muxed container such as .ogg, .webm, or .mkv.";
                    return AVB_ERROR_INVALID_ARGUMENT;
                }
                if (options.audio.bitrate > 0) snprintf(aenc, sizeof(aenc), "vorbisenc bitrate=%d", options.audio.bitrate);
                else          snprintf(aenc, sizeof(aenc), "vorbisenc");
                break;
            case AVB_AUDIO_CODEC_PCM_S16:
                if (!ends_with(".wav")) {
                    m_last_error = "PCM_S16 encoding currently requires a .wav output with GStreamer.";
                    return AVB_ERROR_INVALID_ARGUMENT;
                }
                audio_encoded_by_mux = true;
                audio_caps = "! audio/x-raw,format=S16LE";
                break;
            case AVB_AUDIO_CODEC_PCM_F32:
                if (!ends_with(".wav")) {
                    m_last_error = "PCM_F32 encoding currently requires a .wav output with GStreamer.";
                    return AVB_ERROR_INVALID_ARGUMENT;
                }
                audio_encoded_by_mux = true;
                audio_caps = "! audio/x-raw,format=F32LE";
                break;
            default:
                m_last_error = "Invalid audio codec (use AUTO/AAC/OPUS/MP3/FLAC/VORBIS/PCM_S16/PCM_F32).";
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
        } else if (m_hw_video && m_input_memory == AVB_VIDEO_MEMORY_NATIVE) {
            n += snprintf(desc + n, sizeof(desc) - n,
                "appsrc name=vsrc is-live=false format=time max-bytes=0 block=true max-buffers=4 ! "
                "queue max-size-buffers=4 max-size-bytes=0 max-size-time=0 ! %s ! %s queue ! mux. ", venc, vparse);
        } else if (m_hw_video && m_input_memory == AVB_VIDEO_MEMORY_DMABUF) {
            n += snprintf(desc + n, sizeof(desc) - n,
                "appsrc name=vsrc is-live=false format=time max-bytes=0 block=true max-buffers=4 ! "
                "queue max-size-buffers=4 max-size-bytes=0 max-size-time=0 ! vapostproc ! %s ! %s queue ! mux. ", venc, vparse);
        } else if (m_hw_video) {
            n += snprintf(desc + n, sizeof(desc) - n,
                "appsrc name=vsrc is-live=false format=time max-bytes=0 block=true max-buffers=4 ! videoconvert ! "
                "vapostproc ! %s ! %s queue ! mux. ", venc, vparse);
        } else {
            n += snprintf(desc + n, sizeof(desc) - n,
                "appsrc name=vsrc is-live=false format=time max-bytes=0 ! videoconvert ! "
                "%s ! %s queue ! mux. ", venc, vparse);
        }
    }
    if (options.audio.enable) {
        if (audio_encoded_by_mux) {
            n += snprintf(desc + n, sizeof(desc) - n,
                "appsrc name=asrc is-live=false format=time max-bytes=0 ! audioconvert ! "
                "audioresample %s ! queue ! mux. ", audio_caps);
        } else {
            n += snprintf(desc + n, sizeof(desc) - n,
                "appsrc name=asrc is-live=false format=time max-bytes=0 ! audioconvert ! "
                "audioresample %s ! %s ! queue ! mux. ", audio_caps, aenc);
        }
    }

    GError *err = nullptr;
    m_pipeline = m_gst.gst_parse_launch(desc, &err);
    if (err) {
        char err_msg[512];
        snprintf(err_msg, sizeof(err_msg), "gst_parse_launch failed: %s",
                 err->message ? err->message : "unknown");
        m_last_error = err_msg;
        m_gst.g_clear_error(&err);
        if (m_pipeline) {
            m_gst.gst_object_unref(m_pipeline);
            m_pipeline = nullptr;
        }
        return AVB_ERROR_OPEN_FAILED;
    }
    if (!m_pipeline) {
        m_last_error = "gst_parse_launch failed: unknown";
        return AVB_ERROR_OPEN_FAILED;
    }

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
            if (m_input_memory == AVB_VIDEO_MEMORY_NATIVE) {
                snprintf(caps_str, sizeof(caps_str),
                         "video/x-raw(memory:VASurface),width=%d,height=%d,framerate=%d/1",
                         m_width, m_height, m_fps_n);
            } else if (m_input_memory == AVB_VIDEO_MEMORY_DMABUF) {
                snprintf(caps_str, sizeof(caps_str),
                         "video/x-raw(memory:DMABuf),format=DMA_DRM,width=%d,height=%d,framerate=%d/1",
                         m_width, m_height, m_fps_n);
            } else {
                snprintf(caps_str, sizeof(caps_str),
                         "video/x-raw,format=%s,width=%d,height=%d,framerate=%d/1",
                         vfmt, m_width, m_height, m_fps_n);
            }
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
    double pts = pts_sec >= 0.0      ? pts_sec
               : frame.pts_sec >= 0.0 ? frame.pts_sec
               : (double)m_video_index / m_frame_rate;

    if (m_input_memory == AVB_VIDEO_MEMORY_NATIVE ||
        m_input_memory == AVB_VIDEO_MEMORY_DMABUF) {
        if (frame.memory_type != m_input_memory) {
            m_last_error = "Native/DMABUF video input requires matching frame.memory_type.";
            return AVB_ERROR_INVALID_ARGUMENT;
        }
        GstBuffer *buf = nullptr;
        if (frame.native_handle && frame.native_handle_id == 0) {
            buf = (GstBuffer *)m_gst.gst_mini_object_ref(
                (GstMiniObject *)frame.native_handle);
        } else if (m_input_memory == AVB_VIDEO_MEMORY_DMABUF) {
            avb_result caps_res = update_dmabuf_caps(frame);
            if (caps_res != AVB_OK) return caps_res;
            buf = build_dmabuf_buffer(frame);
            if (!buf) return AVB_ERROR_ENCODE_FAILED;
        } else {
            m_last_error = "Native video input requires a GstBuffer native_handle.";
            return AVB_ERROR_INVALID_ARGUMENT;
        }
        buf->pts = (GstClockTime)std::llround(pts * GST_SECOND);
        buf->duration = (GstClockTime)(GST_SECOND / m_frame_rate);
        GstFlowReturn fr = m_gst.gst_app_src_push_buffer((GstAppSrc *)m_vsrc, buf);
        m_video_index++;
        if (fr != GST_FLOW_OK) {
            if (check_bus_error() == AVB_OK) m_last_error = "appsrc push (native video) failed.";
            return AVB_ERROR_ENCODE_FAILED;
        }
        return AVB_OK;
    }

    if (frame.format != m_input_format) {
        m_last_error = "Frame pixel format does not match configured input_format.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }

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
