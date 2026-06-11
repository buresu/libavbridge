#include "avb_backend_ffmpeg.hpp"
#include "../../avb_video_codec_registry.hpp"

#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>

static constexpr uint32_t avb_drm_fourcc(char a, char b, char c, char d) {
    return (uint32_t)(uint8_t)a |
           ((uint32_t)(uint8_t)b << 8) |
           ((uint32_t)(uint8_t)c << 16) |
           ((uint32_t)(uint8_t)d << 24);
}

static uint32_t avb_infer_drm_frame_format(const AVDRMFrameDescriptor *drm) {
    if (!drm || drm->nb_layers <= 0) return 0;
    if (drm->nb_layers >= 2 &&
        drm->layers[0].format == avb_drm_fourcc('R', '8', ' ', ' ') &&
        drm->layers[1].format == avb_drm_fourcc('G', 'R', '8', '8')) {
        return avb_drm_fourcc('N', 'V', '1', '2');
    }
    return drm->layers[0].format;
}

AvbBackendFFmpeg::AvbBackendFFmpeg() {
    char err_buf[512];
    m_libs_loaded = avb_ffmpeg_load(m_ff, err_buf, sizeof(err_buf));
    if (!m_libs_loaded) {
        m_last_error = err_buf;
    }
}

AvbBackendFFmpeg::~AvbBackendFFmpeg() {
    close_internal();
}

const char *AvbBackendFFmpeg::get_backend_name() const { return "ffmpeg"; }
const char *AvbBackendFFmpeg::get_last_error() const {
    return m_last_error.empty() ? nullptr : m_last_error.c_str();
}

void AvbBackendFFmpeg::set_error(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    m_last_error = buf;
}

void AvbBackendFFmpeg::set_ff_error(const char *prefix, int errnum) {
    char errbuf[256];
    m_ff.av_strerror(errnum, errbuf, sizeof(errbuf));
    set_error("%s: %s", prefix, errbuf);
}

void AvbBackendFFmpeg::clear_packet_queues() {
    for (AVPacket *p : m_audio_pkts) m_ff.av_packet_free(&p);
    for (AVPacket *p : m_video_pkts) m_ff.av_packet_free(&p);
    m_audio_pkts.clear();
    m_video_pkts.clear();
}

AVPacket *AvbBackendFFmpeg::demux_next(int stream_idx) {
    std::deque<AVPacket *> &want_q =
        (stream_idx == m_audio_stream_idx) ? m_audio_pkts : m_video_pkts;
    if (!want_q.empty()) {
        AVPacket *p = want_q.front();
        want_q.pop_front();
        return p;
    }

    while (true) {
        int ret = m_ff.av_read_frame(m_fmt_ctx, m_packet);
        if (ret < 0) return nullptr; // EOF or read error

        int sidx = m_packet->stream_index;
        if (sidx == stream_idx) {
            AVPacket *p = m_ff.av_packet_alloc();
            m_ff.av_packet_move_ref(p, m_packet);
            return p;
        }

        // Queue packets for the other enabled stream; discard everything else.
        if (sidx == m_audio_stream_idx && m_audio_codec_ctx) {
            AVPacket *p = m_ff.av_packet_alloc();
            m_ff.av_packet_move_ref(p, m_packet);
            m_audio_pkts.push_back(p);
        } else if (sidx == m_video_stream_idx &&
                   (m_video_codec_ctx || m_custom_video_decoder)) {
            AVPacket *p = m_ff.av_packet_alloc();
            m_ff.av_packet_move_ref(p, m_packet);
            m_video_pkts.push_back(p);
        } else {
            m_ff.av_packet_unref(m_packet);
        }
    }
}

void AvbBackendFFmpeg::close_internal() {
    if (m_sws) {
        m_ff.sws_freeContext(m_sws);
        m_sws = nullptr;
    }
    if (m_swr) {
        m_ff.swr_free(&m_swr);
        m_swr = nullptr;
    }
    if (m_audio_frame) {
        m_ff.av_frame_free(&m_audio_frame);
        m_audio_frame = nullptr;
    }
    if (m_video_frame) {
        m_ff.av_frame_free(&m_video_frame);
        m_video_frame = nullptr;
    }
    if (m_hw_transfer_frame) {
        m_ff.av_frame_free(&m_hw_transfer_frame);
        m_hw_transfer_frame = nullptr;
    }
    if (m_native_video_frame) {
        m_ff.av_frame_free(&m_native_video_frame);
        m_native_video_frame = nullptr;
    }
    if (m_drm_video_frame) {
        m_ff.av_frame_free(&m_drm_video_frame);
        m_drm_video_frame = nullptr;
    }
    clear_packet_queues();
    if (m_packet) {
        m_ff.av_packet_free(&m_packet);
        m_packet = nullptr;
    }
    if (m_audio_codec_ctx) {
        m_ff.avcodec_free_context(&m_audio_codec_ctx);
        m_audio_codec_ctx = nullptr;
    }
    if (m_video_codec_ctx) {
        m_ff.avcodec_free_context(&m_video_codec_ctx);
        m_video_codec_ctx = nullptr;
    }
    if (m_hw_device_ctx) {
        m_ff.av_buffer_unref(&m_hw_device_ctx);
        m_hw_device_ctx = nullptr;
    }
    if (m_custom_video_decoder) {
        if (m_custom_video_decoder->close && m_custom_video_ctx)
            m_custom_video_decoder->close(m_custom_video_ctx);
        m_custom_video_decoder = nullptr;
        m_custom_video_ctx = nullptr;
    }
    if (m_fmt_ctx) {
        m_ff.avformat_close_input(&m_fmt_ctx);
        m_fmt_ctx = nullptr;
    }
    if (m_avio) {
        // Free the (possibly reallocated) internal buffer, then the context.
        if (m_avio->buffer) m_ff.av_free(m_avio->buffer);
        m_ff.avio_context_free(&m_avio);
        m_avio = nullptr;
    }
    m_io_cb   = avb_io_callbacks{};
    m_io_user = nullptr;
    m_audio_buf.clear();
    m_audio_buf_pos = 0;
    m_audio_buf_pts = -1.0;
    m_video_out_buf.clear();
    m_sws_src_w   = 0;
    m_sws_src_h   = 0;
    m_sws_src_fmt = AV_PIX_FMT_NONE;
    m_hw_pix_fmt = AV_PIX_FMT_NONE;
    m_video_memory = AVB_VIDEO_MEMORY_CPU;
    m_hw_device = AVB_HW_DEVICE_AUTO;
    m_eof = false;
    m_seek_target = -1.0;
    m_audio_seek_target = -1.0;
    m_audio_stream_idx = -1;
    m_video_stream_idx = -1;
    m_custom_video_codec_name.clear();
}

int AvbBackendFFmpeg::ffio_read(void *opaque, uint8_t *buf, int size) {
    auto *self = static_cast<AvbBackendFFmpeg *>(opaque);
    int n = self->m_io_cb.read(self->m_io_user, buf, size);
    if (n > 0)  return n;
    if (n == 0) return AVERROR_EOF;
    return AVERROR(EIO);
}

int64_t AvbBackendFFmpeg::ffio_seek(void *opaque, int64_t offset, int whence) {
    auto *self = static_cast<AvbBackendFFmpeg *>(opaque);
    if (whence == AVSEEK_SIZE)
        return self->m_io_cb.size ? self->m_io_cb.size(self->m_io_user) : -1;
    whence &= ~AVSEEK_FORCE;
    if (!self->m_io_cb.seek) return -1;
    return self->m_io_cb.seek(self->m_io_user, offset, whence);
}

avb_result AvbBackendFFmpeg::open_file(const char *path, const avb_decode_options &options) {
    if (!m_libs_loaded) return AVB_ERROR_BACKEND_NOT_AVAILABLE;

    close_internal();

    int ret = m_ff.avformat_open_input(&m_fmt_ctx, path, nullptr, nullptr);
    if (ret < 0) {
        set_ff_error("avformat_open_input failed", ret);
        return AVB_ERROR_OPEN_FAILED;
    }
    return setup_after_open(options);
}

avb_result AvbBackendFFmpeg::open_io(const avb_io_callbacks &cb, void *user,
                                     const avb_decode_options &options) {
    if (!m_libs_loaded) return AVB_ERROR_BACKEND_NOT_AVAILABLE;
    if (!cb.read) return AVB_ERROR_INVALID_ARGUMENT;

    close_internal();
    m_io_cb   = cb;
    m_io_user = user;

    const int buf_size = 64 * 1024;
    unsigned char *buffer = (unsigned char *)m_ff.av_malloc(buf_size);
    if (!buffer) { set_error("av_malloc failed for AVIO buffer."); return AVB_ERROR_OPEN_FAILED; }

    m_avio = m_ff.avio_alloc_context(buffer, buf_size, 0, this,
                                     &ffio_read, nullptr,
                                     cb.seek ? &ffio_seek : nullptr);
    if (!m_avio) {
        m_ff.av_free(buffer);
        set_error("avio_alloc_context failed.");
        return AVB_ERROR_OPEN_FAILED;
    }

    m_fmt_ctx = m_ff.avformat_alloc_context();
    if (!m_fmt_ctx) { set_error("avformat_alloc_context failed."); return AVB_ERROR_OPEN_FAILED; }
    m_fmt_ctx->pb = m_avio;

    int ret = m_ff.avformat_open_input(&m_fmt_ctx, nullptr, nullptr, nullptr);
    if (ret < 0) {
        set_ff_error("avformat_open_input (custom I/O) failed", ret);
        return AVB_ERROR_OPEN_FAILED;
    }
    return setup_after_open(options);
}

static double avb_ff_seconds(int64_t value, AVRational time_base) {
    if (value == AV_NOPTS_VALUE || time_base.den == 0) return -1.0;
    return (double)value * time_base.num / time_base.den;
}

static std::string avb_ff_codec_tag_name(uint32_t tag) {
    if (tag == 0) return "";
    char s[5];
    s[0] = (char)(tag & 0xff);
    s[1] = (char)((tag >> 8) & 0xff);
    s[2] = (char)((tag >> 16) & 0xff);
    s[3] = (char)((tag >> 24) & 0xff);
    s[4] = '\0';
    for (int i = 0; i < 4; ++i) {
        if ((unsigned char)s[i] < 32 || (unsigned char)s[i] > 126) return "";
    }
    return s;
}

static std::string avb_ff_codec_name(AVCodecID id, const AVCodec *codec, uint32_t tag) {
    switch (id) {
        case AV_CODEC_ID_H264: return "h264";
        case AV_CODEC_ID_HEVC: return "hevc";
        case AV_CODEC_ID_VP8:  return "vp8";
        case AV_CODEC_ID_VP9:  return "vp9";
        case AV_CODEC_ID_AV1:  return "av1";
        case AV_CODEC_ID_HAP:  return "hap";
        case AV_CODEC_ID_AAC:  return "aac";
        case AV_CODEC_ID_OPUS: return "opus";
        default: break;
    }
    if (codec && codec->name) return codec->name;
    return avb_ff_codec_tag_name(tag);
}

static AVHWDeviceType avb_ff_hw_type(avb_hardware_device device) {
    switch (device) {
        case AVB_HW_DEVICE_VAAPI:        return AV_HWDEVICE_TYPE_VAAPI;
        case AVB_HW_DEVICE_CUDA:         return AV_HWDEVICE_TYPE_CUDA;
        case AVB_HW_DEVICE_QSV:          return AV_HWDEVICE_TYPE_QSV;
        case AVB_HW_DEVICE_D3D11VA:      return AV_HWDEVICE_TYPE_D3D11VA;
        case AVB_HW_DEVICE_VIDEOTOOLBOX: return AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
        default:                         return AV_HWDEVICE_TYPE_NONE;
    }
}

static avb_hardware_device avb_ff_hw_device(AVHWDeviceType type) {
    switch (type) {
        case AV_HWDEVICE_TYPE_VAAPI:        return AVB_HW_DEVICE_VAAPI;
        case AV_HWDEVICE_TYPE_CUDA:         return AVB_HW_DEVICE_CUDA;
        case AV_HWDEVICE_TYPE_QSV:          return AVB_HW_DEVICE_QSV;
        case AV_HWDEVICE_TYPE_D3D11VA:      return AVB_HW_DEVICE_D3D11VA;
        case AV_HWDEVICE_TYPE_VIDEOTOOLBOX: return AVB_HW_DEVICE_VIDEOTOOLBOX;
        default:                            return AVB_HW_DEVICE_AUTO;
    }
}

static const AVHWDeviceType *avb_ff_auto_hw_types() {
    static const AVHWDeviceType types[] = {
#if defined(__linux__)
        AV_HWDEVICE_TYPE_VAAPI,
        AV_HWDEVICE_TYPE_CUDA,
        AV_HWDEVICE_TYPE_QSV,
#elif defined(_WIN32)
        AV_HWDEVICE_TYPE_D3D11VA,
        AV_HWDEVICE_TYPE_CUDA,
        AV_HWDEVICE_TYPE_QSV,
#elif defined(__APPLE__)
        AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
#endif
        AV_HWDEVICE_TYPE_NONE
    };
    return types;
}

AVPixelFormat AvbBackendFFmpeg::get_hw_format(AVCodecContext *ctx,
                                              const AVPixelFormat *pix_fmts) {
    auto *self = static_cast<AvbBackendFFmpeg *>(ctx->opaque);
    if (!self) return pix_fmts[0];
    for (const AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == self->m_hw_pix_fmt) return *p;
    }
    return pix_fmts[0];
}

avb_result AvbBackendFFmpeg::setup_hardware_decoder(
    const AVCodec *codec,
    const avb_decode_options &options
) {
    if (options.hardware_policy == AVB_HARDWARE_DISABLED &&
        options.video_memory == AVB_VIDEO_MEMORY_CPU) {
        return AVB_ERROR_STREAM_NOT_FOUND;
    }

    AVHWDeviceType requested = avb_ff_hw_type(options.hardware_device);
    const AVHWDeviceType *auto_types = avb_ff_auto_hw_types();

    for (int i = 0;; ++i) {
        const AVCodecHWConfig *cfg = m_ff.avcodec_get_hw_config(codec, i);
        if (!cfg) break;
        if (!(cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) continue;

        bool type_ok = false;
        if (requested != AV_HWDEVICE_TYPE_NONE) {
            type_ok = cfg->device_type == requested;
        } else {
            for (const AVHWDeviceType *t = auto_types; *t != AV_HWDEVICE_TYPE_NONE; ++t) {
                if (cfg->device_type == *t) { type_ok = true; break; }
            }
        }
        if (!type_ok) continue;

        AVBufferRef *device = nullptr;
        int ret = m_ff.av_hwdevice_ctx_create(&device, cfg->device_type, nullptr, nullptr, 0);
        if (ret < 0) continue;

        m_hw_pix_fmt = cfg->pix_fmt;
        m_hw_device = avb_ff_hw_device(cfg->device_type);
        m_hw_device_ctx = device;
        m_video_codec_ctx->hw_device_ctx = m_ff.av_buffer_ref(m_hw_device_ctx);
        m_video_codec_ctx->get_format = &AvbBackendFFmpeg::get_hw_format;
        m_video_codec_ctx->opaque = this;
        return AVB_OK;
    }

    return AVB_ERROR_STREAM_NOT_FOUND;
}

avb_result AvbBackendFFmpeg::open_custom_video_decoder(
    AVStream *st,
    const avb_decode_options &options
) {
    avb_video_stream_info stream{};
    stream.stream_index = m_video_stream_idx;
    stream.width = st->codecpar->width;
    stream.height = st->codecpar->height;
    stream.codec_tag = (uint32_t)st->codecpar->codec_tag;
    stream.extradata = st->codecpar->extradata;
    stream.extradata_size = st->codecpar->extradata_size;
    stream.time_base_num = st->time_base.num;
    stream.time_base_den = st->time_base.den;
    if (st->avg_frame_rate.den != 0)
        stream.frame_rate = (double)st->avg_frame_rate.num / st->avg_frame_rate.den;
    stream.duration_sec = avb_ff_seconds(st->duration, st->time_base);

    const AVCodec *codec = m_ff.avcodec_find_decoder(st->codecpar->codec_id);
    m_custom_video_codec_name = avb_ff_codec_name(
        st->codecpar->codec_id, codec, stream.codec_tag);
    stream.codec_name = m_custom_video_codec_name.empty()
        ? nullptr : m_custom_video_codec_name.c_str();

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
    return AVB_OK;
}

avb_result AvbBackendFFmpeg::setup_after_open(const avb_decode_options &options) {
    int ret = m_ff.avformat_find_stream_info(m_fmt_ctx, nullptr);
    if (ret < 0) {
        set_ff_error("avformat_find_stream_info failed", ret);
        return AVB_ERROR_OPEN_FAILED;
    }

    if (options.enable_audio) {
        m_audio_stream_idx = (options.audio_stream_index >= 0)
            ? options.audio_stream_index
            : m_ff.av_find_best_stream(m_fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    }

    if (options.enable_video) {
        m_video_stream_idx = (options.video_stream_index >= 0)
            ? options.video_stream_index
            : m_ff.av_find_best_stream(m_fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    }

    if (m_audio_stream_idx < 0 && m_video_stream_idx < 0) {
        set_error("No audio or video stream found.");
        return AVB_ERROR_STREAM_NOT_FOUND;
    }

    // Open audio decoder
    if (m_audio_stream_idx >= 0) {
        AVStream *st = m_fmt_ctx->streams[m_audio_stream_idx];
        const AVCodec *codec = m_ff.avcodec_find_decoder(st->codecpar->codec_id);
        if (!codec) {
            set_error("No audio decoder found for codec id %d.", st->codecpar->codec_id);
            return AVB_ERROR_STREAM_NOT_FOUND;
        }
        m_audio_codec_name = avb_ff_codec_name(
            st->codecpar->codec_id, codec, (uint32_t)st->codecpar->codec_tag);

        m_audio_codec_ctx = m_ff.avcodec_alloc_context3(codec);
        if (!m_audio_codec_ctx) { set_error("avcodec_alloc_context3 failed."); return AVB_ERROR_OPEN_FAILED; }

        ret = m_ff.avcodec_parameters_to_context(m_audio_codec_ctx, st->codecpar);
        if (ret < 0) { set_ff_error("avcodec_parameters_to_context (audio)", ret); return AVB_ERROR_OPEN_FAILED; }

        ret = m_ff.avcodec_open2(m_audio_codec_ctx, codec, nullptr);
        if (ret < 0) { set_ff_error("avcodec_open2 (audio)", ret); return AVB_ERROR_OPEN_FAILED; }

        m_swr = m_ff.swr_alloc();
        if (!m_swr) { set_error("swr_alloc failed."); return AVB_ERROR_OPEN_FAILED; }

        // Effective output format: requested override (options), else source.
        m_out_sample_rate = options.audio_sample_rate > 0
            ? options.audio_sample_rate : m_audio_codec_ctx->sample_rate;

        AVChannelLayout out_layout;
        if (options.audio_channels > 0) {
            m_ff.av_channel_layout_default(&out_layout, options.audio_channels);
        } else {
            m_ff.av_channel_layout_copy(&out_layout, &m_audio_codec_ctx->ch_layout);
        }
        m_out_channels = out_layout.nb_channels;

        ret = m_ff.swr_alloc_set_opts2(
            &m_swr,
            &out_layout, AV_SAMPLE_FMT_FLT, m_out_sample_rate,
            &m_audio_codec_ctx->ch_layout, m_audio_codec_ctx->sample_fmt, m_audio_codec_ctx->sample_rate,
            0, nullptr);
        m_ff.av_channel_layout_uninit(&out_layout);
        if (ret < 0) { set_ff_error("swr_alloc_set_opts2", ret); return AVB_ERROR_OPEN_FAILED; }

        ret = m_ff.swr_init(m_swr);
        if (ret < 0) { set_ff_error("swr_init", ret); return AVB_ERROR_OPEN_FAILED; }
    }

    // Open video decoder
    if (m_video_stream_idx >= 0) {
        AVStream *st = m_fmt_ctx->streams[m_video_stream_idx];
        avb_result custom_res = options.enable_custom_video_decoders
            ? open_custom_video_decoder(st, options)
            : AVB_ERROR_STREAM_NOT_FOUND;
        if (custom_res == AVB_OK) {
            m_video_codec_name = m_custom_video_codec_name;
        } else {
            if (custom_res != AVB_ERROR_STREAM_NOT_FOUND) return custom_res;
            const AVCodec *codec = m_ff.avcodec_find_decoder(st->codecpar->codec_id);
            if (!codec) {
                set_error("No video decoder found for codec id %d.", st->codecpar->codec_id);
                return AVB_ERROR_STREAM_NOT_FOUND;
            }
            m_video_codec_name = avb_ff_codec_name(
                st->codecpar->codec_id, codec, (uint32_t)st->codecpar->codec_tag);

            m_video_codec_ctx = m_ff.avcodec_alloc_context3(codec);
            if (!m_video_codec_ctx) {
                set_error("avcodec_alloc_context3 (video) failed.");
                return AVB_ERROR_OPEN_FAILED;
            }

            ret = m_ff.avcodec_parameters_to_context(m_video_codec_ctx, st->codecpar);
            if (ret < 0) {
                set_ff_error("avcodec_parameters_to_context (video)", ret);
                return AVB_ERROR_OPEN_FAILED;
            }

            m_video_memory = options.video_memory;
            avb_result hw_res = setup_hardware_decoder(codec, options);
            if (hw_res != AVB_OK &&
                (options.hardware_policy == AVB_HARDWARE_REQUIRE ||
                 options.video_memory == AVB_VIDEO_MEMORY_DMABUF)) {
                set_error("Required FFmpeg hardware decoder is not available for this stream.");
                return AVB_ERROR_OPEN_FAILED;
            }

            ret = m_ff.avcodec_open2(m_video_codec_ctx, codec, nullptr);
            if (ret < 0) {
                set_ff_error("avcodec_open2 (video)", ret);
                return AVB_ERROR_OPEN_FAILED;
            }

            // Resolve the requested output pixel format (UNKNOWN -> BGRA8 default).
            switch (options.video_format) {
                case AVB_PIXEL_FORMAT_RGBA8:
                    m_video_format = AVB_PIXEL_FORMAT_RGBA8; m_dst_av_fmt = AV_PIX_FMT_RGBA; break;
                case AVB_PIXEL_FORMAT_NV12:
                    m_video_format = AVB_PIXEL_FORMAT_NV12;  m_dst_av_fmt = AV_PIX_FMT_NV12; break;
                case AVB_PIXEL_FORMAT_I420:
                    m_video_format = AVB_PIXEL_FORMAT_I420;  m_dst_av_fmt = AV_PIX_FMT_YUV420P; break;
                case AVB_PIXEL_FORMAT_BGRA8:
                case AVB_PIXEL_FORMAT_UNKNOWN:
                    m_video_format = AVB_PIXEL_FORMAT_BGRA8; m_dst_av_fmt = AV_PIX_FMT_BGRA; break;
                default:
                    set_error("Requested video pixel format is not supported by FFmpeg conversion.");
                    return AVB_ERROR_INVALID_ARGUMENT;
            }
        }
    }

    m_packet      = m_ff.av_packet_alloc();
    m_audio_frame = m_ff.av_frame_alloc();
    m_video_frame = m_ff.av_frame_alloc();
    m_hw_transfer_frame = m_ff.av_frame_alloc();
    m_native_video_frame = m_ff.av_frame_alloc();
    m_drm_video_frame = m_ff.av_frame_alloc();
    if (!m_packet || !m_audio_frame || !m_video_frame ||
        !m_hw_transfer_frame || !m_native_video_frame || !m_drm_video_frame) {
        set_error("Memory allocation failed.");
        return AVB_ERROR_OPEN_FAILED;
    }

    // Build cached media info
    m_media_info = {};
    m_media_info.backend_name = "ffmpeg";
    if (m_fmt_ctx->duration != AV_NOPTS_VALUE)
        m_media_info.duration_sec = (double)m_fmt_ctx->duration / AV_TIME_BASE;

    if (m_audio_stream_idx >= 0) {
        AVStream *st = m_fmt_ctx->streams[m_audio_stream_idx];
        unsigned audio_streams = 0;
        for (unsigned i = 0; i < m_fmt_ctx->nb_streams; ++i)
            if (m_fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
                ++audio_streams;
        m_media_info.audio.available    = 1;
        m_media_info.audio.stream_index = m_audio_stream_idx;
        m_media_info.audio.track_count  = (int)audio_streams;
        m_media_info.audio.sample_rate  = m_out_sample_rate;
        m_media_info.audio.channels     = m_out_channels;
        m_media_info.audio.codec_name   = m_audio_codec_name.c_str();
        m_media_info.audio.duration_sec =
            (st->duration != AV_NOPTS_VALUE && st->time_base.den != 0)
            ? (double)st->duration * st->time_base.num / st->time_base.den
            : m_media_info.duration_sec;
    }

    if (m_video_stream_idx >= 0) {
        AVStream *st = m_fmt_ctx->streams[m_video_stream_idx];
        m_media_info.video.available    = 1;
        m_media_info.video.stream_index = m_video_stream_idx;
        // Use codecpar for dimensions — avcodec_ctx may not be populated until first decoded frame
        m_media_info.video.width        = st->codecpar->width;
        m_media_info.video.height       = st->codecpar->height;
        m_media_info.video.codec_name   = m_video_codec_name.c_str();
        if (st->avg_frame_rate.den != 0)
            m_media_info.video.frame_rate =
                (double)st->avg_frame_rate.num / st->avg_frame_rate.den;
        m_media_info.video.duration_sec =
            (st->duration != AV_NOPTS_VALUE && st->time_base.den != 0)
            ? (double)st->duration * st->time_base.num / st->time_base.den
            : m_media_info.duration_sec;
    }

    return AVB_OK;
}

avb_result AvbBackendFFmpeg::get_media_info(avb_media_info &out_info) {
    if (!m_fmt_ctx) return AVB_ERROR_INVALID_ARGUMENT;
    out_info = m_media_info;
    return AVB_OK;
}

avb_result AvbBackendFFmpeg::seek(double seconds) {
    if (!m_fmt_ctx) return AVB_ERROR_INVALID_ARGUMENT;

    int64_t ts = (int64_t)(seconds * AV_TIME_BASE);
    int ret = m_ff.av_seek_frame(m_fmt_ctx, -1, ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        set_ff_error("av_seek_frame failed", ret);
        return AVB_ERROR_SEEK_FAILED;
    }

    if (m_audio_codec_ctx) m_ff.avcodec_flush_buffers(m_audio_codec_ctx);
    if (m_video_codec_ctx) m_ff.avcodec_flush_buffers(m_video_codec_ctx);
    if (m_custom_video_decoder && m_custom_video_decoder->flush)
        m_custom_video_decoder->flush(m_custom_video_ctx);

    clear_packet_queues();
    m_audio_buf.clear();
    m_audio_buf_pos = 0;
    m_audio_buf_pts = -1.0;
    m_eof = false;
    m_seek_target = seconds;
    m_audio_seek_target = seconds;
    return AVB_OK;
}

bool AvbBackendFFmpeg::fill_audio_buffer() {
    if (!m_audio_codec_ctx) return false;

    while (true) {
        int ret = m_ff.avcodec_receive_frame(m_audio_codec_ctx, m_audio_frame);
        if (ret == 0) {
            int nb_channels = m_out_channels;
            int in_samples  = m_audio_frame->nb_samples;
            int in_rate     = m_audio_codec_ctx->sample_rate;

            // Frame presentation time (seconds), or -1 if unknown.
            AVStream *ast = m_fmt_ctx->streams[m_audio_stream_idx];
            double frame_pts = -1.0;
            if (m_audio_frame->pts != AV_NOPTS_VALUE && ast->time_base.den != 0)
                frame_pts = (double)m_audio_frame->pts * ast->time_base.num / ast->time_base.den;

            // After a seek, drop whole frames that end before the target so audio
            // resumes at ~target (matching the trimmed video path).
            if (m_audio_seek_target >= 0.0 && frame_pts >= 0.0 && in_rate > 0) {
                double frame_end = frame_pts + (double)in_samples / in_rate;
                if (frame_end <= m_audio_seek_target) {
                    m_ff.av_frame_unref(m_audio_frame);
                    continue;
                }
            }
            m_audio_seek_target = -1.0;

            // Output sample count after resampling can differ from the input;
            // size for buffered delay + this frame, rounded up.
            int64_t delay = m_ff.swr_get_delay(m_swr, in_rate);
            int out_capacity =
                (int)(((int64_t)in_samples + delay) * m_out_sample_rate / in_rate) + 1;

            int buf_start = (int)m_audio_buf.size();

            // When this frame becomes the head of an empty buffer, record its
            // presentation time so audio_next_pts() can report it.
            if (buf_start == 0) m_audio_buf_pts = frame_pts;

            m_audio_buf.resize(buf_start + out_capacity * nb_channels);
            float *dst = m_audio_buf.data() + buf_start;

            uint8_t *dst_ptr = (uint8_t *)dst;
            int converted = m_ff.swr_convert(m_swr, &dst_ptr, out_capacity,
                (const uint8_t **)m_audio_frame->data, in_samples);
            if (converted < 0) {
                m_audio_buf.resize(buf_start);
                set_error("swr_convert failed.");
                return false;
            }
            // Shrink to the actual number of converted samples.
            m_audio_buf.resize(buf_start + converted * nb_channels);

            m_ff.av_frame_unref(m_audio_frame);
            return true;
        }

        if (ret != AVERROR(EAGAIN)) {
            if (ret == AVERROR_EOF) m_eof = true;
            return false;
        }

        AVPacket *pkt = demux_next(m_audio_stream_idx);
        if (!pkt) {
            // End of demuxed input: flush the decoder so the loop can drain any
            // remaining buffered frames (then receive_frame returns EOF above).
            m_ff.avcodec_send_packet(m_audio_codec_ctx, nullptr);
            continue;
        }
        int send_ret = m_ff.avcodec_send_packet(m_audio_codec_ctx, pkt);
        m_ff.av_packet_free(&pkt);
        if (send_ret < 0 && send_ret != AVERROR(EAGAIN)) {
            set_ff_error("avcodec_send_packet (audio)", send_ret);
            return false;
        }
    }
}

int AvbBackendFFmpeg::read_audio_f32(float *dst_interleaved, int frames) {
    if (!m_audio_codec_ctx || m_eof) return 0;

    int nb_channels    = m_out_channels;
    int samples_needed = frames * nb_channels;
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
            // Advance the head timestamp by the number of frames consumed.
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

double AvbBackendFFmpeg::audio_next_pts() {
    if (!m_audio_codec_ctx) return -1.0;
    // Ensure the buffer holds the next sample, then report its timestamp.
    if (m_audio_buf_pos >= (int)m_audio_buf.size()) {
        if (!fill_audio_buffer()) return -1.0;
    }
    return m_audio_buf_pts;
}

avb_result AvbBackendFFmpeg::read_custom_video_frame(avb_video_frame &out_frame) {
    if (!m_custom_video_decoder || !m_custom_video_ctx)
        return AVB_ERROR_STREAM_NOT_FOUND;

    while (true) {
        AVPacket *pkt = demux_next(m_video_stream_idx);
        if (!pkt) return AVB_ERROR_EOF;

        AVStream *vst = m_fmt_ctx->streams[m_video_stream_idx];
        double packet_pts = avb_ff_seconds(pkt->pts, vst->time_base);
        if (packet_pts < 0.0)
            packet_pts = avb_ff_seconds(pkt->dts, vst->time_base);

        // Drop pre-roll packets after seeking. HAP-like intra-frame codecs can
        // resume directly from the first packet at/after the target.
        if (m_seek_target >= 0.0 && packet_pts >= 0.0 &&
            packet_pts < m_seek_target - 1e-3) {
            m_ff.av_packet_free(&pkt);
            continue;
        }
        m_seek_target = -1.0;

        avb_encoded_packet packet{};
        packet.data = pkt->data;
        packet.size = pkt->size;
        packet.pts_sec = packet_pts;
        packet.duration_sec = avb_ff_seconds(pkt->duration, vst->time_base);
        packet.keyframe = (pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0;
        packet.stream_index = pkt->stream_index;
        packet.pts = pkt->pts;
        packet.dts = pkt->dts;
        packet.duration = pkt->duration;
        packet.time_base_num = vst->time_base.num;
        packet.time_base_den = vst->time_base.den;

        avb_result res = m_custom_video_decoder->decode_packet(
            m_custom_video_ctx, &packet, &out_frame);
        m_ff.av_packet_free(&pkt);
        if (res == AVB_OK) {
            if (out_frame.pts_sec < 0.0) out_frame.pts_sec = packet_pts;
            return AVB_OK;
        }
        if (res == AVB_ERROR_AGAIN) continue;
        return res;
    }
}

avb_result AvbBackendFFmpeg::fill_native_video_frame(
    AVFrame *frame,
    double pts_sec,
    avb_video_frame &out_frame
) {
    m_ff.av_frame_unref(m_native_video_frame);
    int ret = m_ff.av_frame_ref(m_native_video_frame, frame);
    if (ret < 0) {
        set_ff_error("av_frame_ref (hardware video)", ret);
        return AVB_ERROR_DECODE_FAILED;
    }

    out_frame = {};
    out_frame.width = frame->width;
    out_frame.height = frame->height;
    out_frame.format = m_video_format;
    out_frame.pts_sec = pts_sec;
    out_frame.memory_type = AVB_VIDEO_MEMORY_NATIVE;
    out_frame.hardware_device = m_hw_device;
    out_frame.plane_count = 0;
    out_frame.native_handle = m_native_video_frame;
    out_frame.native_owner = this;
    if ((AVPixelFormat)frame->format == AV_PIX_FMT_VAAPI) {
        out_frame.native_handle_id = (uint64_t)(uintptr_t)frame->data[3];
    }
    for (int p = 0; p < AVB_MAX_PLANES; ++p) out_frame.dmabuf_fd[p] = -1;
    return AVB_OK;
}

avb_result AvbBackendFFmpeg::fill_dmabuf_video_frame(
    AVFrame *frame,
    double pts_sec,
    avb_video_frame &out_frame
) {
    m_ff.av_frame_unref(m_drm_video_frame);
    m_drm_video_frame->format = AV_PIX_FMT_DRM_PRIME;
    int ret = m_ff.av_hwframe_map(m_drm_video_frame, frame, AV_HWFRAME_MAP_READ);
    if (ret < 0) {
        set_ff_error("av_hwframe_map (DRM PRIME video)", ret);
        return AVB_ERROR_DECODE_FAILED;
    }
    if ((AVPixelFormat)m_drm_video_frame->format != AV_PIX_FMT_DRM_PRIME ||
        !m_drm_video_frame->data[0]) {
        set_error("FFmpeg hardware frame did not map to DRM PRIME.");
        m_ff.av_frame_unref(m_drm_video_frame);
        return AVB_ERROR_DECODE_FAILED;
    }

    const AVDRMFrameDescriptor *drm =
        (const AVDRMFrameDescriptor *)m_drm_video_frame->data[0];
    if (drm->nb_layers <= 0 || drm->layers[0].nb_planes <= 0) {
        set_error("FFmpeg DRM PRIME frame has no planes.");
        m_ff.av_frame_unref(m_drm_video_frame);
        return AVB_ERROR_DECODE_FAILED;
    }

    out_frame = {};
    out_frame.width = frame->width;
    out_frame.height = frame->height;
    out_frame.format = AVB_PIXEL_FORMAT_UNKNOWN;
    out_frame.pts_sec = pts_sec;
    out_frame.memory_type = AVB_VIDEO_MEMORY_DMABUF;
    out_frame.hardware_device = m_hw_device;
    out_frame.native_handle = m_drm_video_frame;
    out_frame.native_owner = this;
    out_frame.drm_format = avb_infer_drm_frame_format(drm);
    out_frame.native_handle_id = out_frame.drm_format;
    for (int p = 0; p < AVB_MAX_PLANES; ++p) out_frame.dmabuf_fd[p] = -1;

    int plane_count = 0;
    for (int layer = 0; layer < drm->nb_layers && plane_count < AVB_MAX_PLANES; ++layer) {
        const AVDRMLayerDescriptor &dl = drm->layers[layer];
        for (int p = 0; p < dl.nb_planes && plane_count < AVB_MAX_PLANES; ++p) {
            const AVDRMPlaneDescriptor &dp = dl.planes[p];
            if (dp.object_index < 0 || dp.object_index >= drm->nb_objects) continue;
            const AVDRMObjectDescriptor &obj = drm->objects[dp.object_index];
            out_frame.dmabuf_fd[plane_count] = obj.fd;
            out_frame.plane_offset[plane_count] = (int)dp.offset;
            out_frame.plane_stride[plane_count] = (int)dp.pitch;
            out_frame.dmabuf_modifier[plane_count] = obj.format_modifier;
            ++plane_count;
        }
    }
    out_frame.plane_count = plane_count;
    out_frame.stride = plane_count > 0 ? out_frame.plane_stride[0] : 0;
    return plane_count > 0 ? AVB_OK : AVB_ERROR_DECODE_FAILED;
}

avb_result AvbBackendFFmpeg::fill_cpu_video_frame(
    AVFrame *frame,
    double pts_sec,
    avb_video_frame &out_frame
) {
    int w = frame->width;
    int h = frame->height;
    AVPixelFormat src_fmt = (AVPixelFormat)frame->format;

    // Rebuild swscale context if source properties changed
    if (!m_sws || m_sws_src_w != w || m_sws_src_h != h || m_sws_src_fmt != src_fmt) {
        if (m_sws) m_ff.sws_freeContext(m_sws);
        m_sws = m_ff.sws_getContext(
            w, h, src_fmt,
            w, h, m_dst_av_fmt,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_sws) {
            set_error("sws_getContext failed (unsupported pixel format?).");
            return AVB_ERROR_DECODE_FAILED;
        }
        m_sws_src_w   = w;
        m_sws_src_h   = h;
        m_sws_src_fmt = src_fmt;
    }

    // Lay out the destination buffer per output format:
    //   RGBA/BGRA: 1 packed plane (stride w*4)
    //   NV12:      2 planes — Y (w), interleaved CbCr (w) at half height
    //   I420:      3 planes — Y (w), Cb (w/2), Cr (w/2) at half height
    int      plane_count = 1;
    int      dst_stride[AVB_MAX_PLANES] = {0, 0, 0};
    int      plane_rows[AVB_MAX_PLANES] = {0, 0, 0};
    switch (m_video_format) {
        case AVB_PIXEL_FORMAT_NV12:
            plane_count = 2;
            dst_stride[0] = w;     plane_rows[0] = h;
            dst_stride[1] = w;     plane_rows[1] = h / 2;
            break;
        case AVB_PIXEL_FORMAT_I420:
            plane_count = 3;
            dst_stride[0] = w;     plane_rows[0] = h;
            dst_stride[1] = w / 2; plane_rows[1] = h / 2;
            dst_stride[2] = w / 2; plane_rows[2] = h / 2;
            break;
        default: // RGBA8 / BGRA8
            plane_count = 1;
            dst_stride[0] = w * 4; plane_rows[0] = h;
            break;
    }

    size_t plane_off[AVB_MAX_PLANES] = {0, 0, 0};
    size_t total = 0;
    for (int p = 0; p < plane_count; ++p) {
        plane_off[p] = total;
        total += (size_t)dst_stride[p] * plane_rows[p];
    }
    m_video_out_buf.resize(total);

    uint8_t *dst_data[AVB_MAX_PLANES] = {nullptr, nullptr, nullptr};
    for (int p = 0; p < plane_count; ++p)
        dst_data[p] = m_video_out_buf.data() + plane_off[p];
    m_ff.sws_scale(m_sws,
        (const uint8_t *const *)frame->data, frame->linesize,
        0, h, dst_data, dst_stride);

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
        out_frame.plane_data[p]   = dst_data[p];
        out_frame.plane_stride[p] = dst_stride[p];
        out_frame.plane_offset[p] = (int)plane_off[p];
    }
    out_frame.data      = out_frame.plane_data[0];
    out_frame.stride    = out_frame.plane_stride[0];
    out_frame.data_size = (int)m_video_out_buf.size();
    return AVB_OK;
}

avb_result AvbBackendFFmpeg::read_video_frame(avb_video_frame &out_frame) {
    if (m_custom_video_decoder) return read_custom_video_frame(out_frame);
    if (!m_video_codec_ctx) return AVB_ERROR_STREAM_NOT_FOUND;

    while (true) {
        int ret = m_ff.avcodec_receive_frame(m_video_codec_ctx, m_video_frame);
        if (ret == 0) {
            // Compute PTS first so pre-roll frames can be skipped cheaply
            // (before any pixel conversion) after a seek.
            AVStream *vst = m_fmt_ctx->streams[m_video_stream_idx];
            double frame_pts = 0.0;
            if (m_video_frame->pts != AV_NOPTS_VALUE && vst->time_base.den != 0)
                frame_pts = (double)m_video_frame->pts * vst->time_base.num / vst->time_base.den;

            // Drop frames decoded between the seek keyframe and the requested
            // target time. A small epsilon avoids dropping the frame that sits
            // essentially on the target due to time-base rounding.
            if (m_seek_target >= 0.0 && frame_pts < m_seek_target - 1e-3) {
                m_ff.av_frame_unref(m_video_frame);
                continue;
            }
            m_seek_target = -1.0;

            AVFrame *output_frame = m_video_frame;
            bool is_hw_frame = m_hw_pix_fmt != AV_PIX_FMT_NONE &&
                (AVPixelFormat)m_video_frame->format == m_hw_pix_fmt;

            avb_result frame_res = AVB_OK;
            if (is_hw_frame && m_video_memory == AVB_VIDEO_MEMORY_NATIVE) {
                frame_res = fill_native_video_frame(m_video_frame, frame_pts, out_frame);
            } else if (is_hw_frame && m_video_memory == AVB_VIDEO_MEMORY_DMABUF) {
                frame_res = fill_dmabuf_video_frame(m_video_frame, frame_pts, out_frame);
            } else {
                if (is_hw_frame) {
                    m_ff.av_frame_unref(m_hw_transfer_frame);
                    int transfer_res = m_ff.av_hwframe_transfer_data(
                        m_hw_transfer_frame, m_video_frame, 0);
                    if (transfer_res < 0) {
                        m_ff.av_frame_unref(m_video_frame);
                        set_ff_error("av_hwframe_transfer_data (video)", transfer_res);
                        return AVB_ERROR_DECODE_FAILED;
                    }
                    m_hw_transfer_frame->pts = m_video_frame->pts;
                    output_frame = m_hw_transfer_frame;
                }
                frame_res = fill_cpu_video_frame(output_frame, frame_pts, out_frame);
            }

            m_ff.av_frame_unref(m_video_frame);
            if (output_frame == m_hw_transfer_frame) m_ff.av_frame_unref(m_hw_transfer_frame);
            return frame_res;
        }

        if (ret != AVERROR(EAGAIN)) {
            if (ret == AVERROR_EOF) return AVB_ERROR_EOF;
            set_ff_error("avcodec_receive_frame (video)", ret);
            return AVB_ERROR_DECODE_FAILED;
        }

        // Feed more packets. demux_next() preserves audio packets seen along the
        // way so a later read_audio_f32() can still consume them.
        AVPacket *pkt = demux_next(m_video_stream_idx);
        if (!pkt) {
            // End of demuxed input: flush, then loop to drain buffered frames
            // (receive_frame above eventually returns AVERROR_EOF).
            m_ff.avcodec_send_packet(m_video_codec_ctx, nullptr);
            continue;
        }
        int send_ret = m_ff.avcodec_send_packet(m_video_codec_ctx, pkt);
        m_ff.av_packet_free(&pkt);
        if (send_ret < 0 && send_ret != AVERROR(EAGAIN)) {
            set_ff_error("avcodec_send_packet (video)", send_ret);
            return AVB_ERROR_DECODE_FAILED;
        }
    }
}

void AvbBackendFFmpeg::release_video_frame(avb_video_frame &frame) {
    if (m_custom_video_decoder) {
        if (m_custom_video_decoder->release_frame)
            m_custom_video_decoder->release_frame(m_custom_video_ctx, &frame);
        memset(&frame, 0, sizeof(frame));
        return;
    }
    if (frame.memory_type == AVB_VIDEO_MEMORY_NATIVE &&
        frame.native_owner == this && m_native_video_frame) {
        m_ff.av_frame_unref(m_native_video_frame);
    }
    if (frame.memory_type == AVB_VIDEO_MEMORY_DMABUF &&
        frame.native_owner == this && m_drm_video_frame) {
        m_ff.av_frame_unref(m_drm_video_frame);
    }
    // Output buffer is owned by the backend and reused on the next read_video_frame call.
    // Just zero the caller's frame struct so they can't accidentally use stale pointers.
    memset(&frame, 0, sizeof(frame));
}
