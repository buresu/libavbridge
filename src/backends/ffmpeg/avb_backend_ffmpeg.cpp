#include "avb_backend_ffmpeg.hpp"

#include <cstdio>
#include <cstdarg>
#include <cstring>

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
    if (m_fmt_ctx) {
        m_ff.avformat_close_input(&m_fmt_ctx);
        m_fmt_ctx = nullptr;
    }
    m_audio_buf.clear();
    m_audio_buf_pos = 0;
    m_video_out_buf.clear();
    m_sws_src_w   = 0;
    m_sws_src_h   = 0;
    m_sws_src_fmt = AV_PIX_FMT_NONE;
    m_eof = false;
    m_audio_stream_idx = -1;
    m_video_stream_idx = -1;
}

avb_result AvbBackendFFmpeg::open_file(const char *path, const avb_open_options &options) {
    if (!m_libs_loaded) return AVB_ERROR_BACKEND_NOT_AVAILABLE;

    close_internal();

    int ret = m_ff.avformat_open_input(&m_fmt_ctx, path, nullptr, nullptr);
    if (ret < 0) {
        set_ff_error("avformat_open_input failed", ret);
        return AVB_ERROR_OPEN_FAILED;
    }

    ret = m_ff.avformat_find_stream_info(m_fmt_ctx, nullptr);
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
        m_audio_codec_name = codec->name ? codec->name : "";

        m_audio_codec_ctx = m_ff.avcodec_alloc_context3(codec);
        if (!m_audio_codec_ctx) { set_error("avcodec_alloc_context3 failed."); return AVB_ERROR_OPEN_FAILED; }

        ret = m_ff.avcodec_parameters_to_context(m_audio_codec_ctx, st->codecpar);
        if (ret < 0) { set_ff_error("avcodec_parameters_to_context (audio)", ret); return AVB_ERROR_OPEN_FAILED; }

        ret = m_ff.avcodec_open2(m_audio_codec_ctx, codec, nullptr);
        if (ret < 0) { set_ff_error("avcodec_open2 (audio)", ret); return AVB_ERROR_OPEN_FAILED; }

        m_swr = m_ff.swr_alloc();
        if (!m_swr) { set_error("swr_alloc failed."); return AVB_ERROR_OPEN_FAILED; }

        AVChannelLayout out_layout;
        m_ff.av_channel_layout_copy(&out_layout, &m_audio_codec_ctx->ch_layout);
        ret = m_ff.swr_alloc_set_opts2(
            &m_swr,
            &out_layout, AV_SAMPLE_FMT_FLT, m_audio_codec_ctx->sample_rate,
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
        const AVCodec *codec = m_ff.avcodec_find_decoder(st->codecpar->codec_id);
        if (!codec) {
            set_error("No video decoder found for codec id %d.", st->codecpar->codec_id);
            return AVB_ERROR_STREAM_NOT_FOUND;
        }
        m_video_codec_name = codec->name ? codec->name : "";

        m_video_codec_ctx = m_ff.avcodec_alloc_context3(codec);
        if (!m_video_codec_ctx) { set_error("avcodec_alloc_context3 (video) failed."); return AVB_ERROR_OPEN_FAILED; }

        ret = m_ff.avcodec_parameters_to_context(m_video_codec_ctx, st->codecpar);
        if (ret < 0) { set_ff_error("avcodec_parameters_to_context (video)", ret); return AVB_ERROR_OPEN_FAILED; }

        ret = m_ff.avcodec_open2(m_video_codec_ctx, codec, nullptr);
        if (ret < 0) { set_ff_error("avcodec_open2 (video)", ret); return AVB_ERROR_OPEN_FAILED; }
    }

    m_packet      = m_ff.av_packet_alloc();
    m_audio_frame = m_ff.av_frame_alloc();
    m_video_frame = m_ff.av_frame_alloc();
    if (!m_packet || !m_audio_frame || !m_video_frame) {
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
        m_media_info.audio.available    = 1;
        m_media_info.audio.stream_index = m_audio_stream_idx;
        m_media_info.audio.sample_rate  = m_audio_codec_ctx->sample_rate;
        m_media_info.audio.channels     = m_audio_codec_ctx->ch_layout.nb_channels;
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

    m_audio_buf.clear();
    m_audio_buf_pos = 0;
    m_eof = false;
    return AVB_OK;
}

bool AvbBackendFFmpeg::fill_audio_buffer() {
    if (!m_audio_codec_ctx) return false;

    while (true) {
        int ret = m_ff.avcodec_receive_frame(m_audio_codec_ctx, m_audio_frame);
        if (ret == 0) {
            int nb_channels = m_audio_codec_ctx->ch_layout.nb_channels;
            int nb_samples  = m_audio_frame->nb_samples;

            int buf_start = (int)m_audio_buf.size();
            m_audio_buf.resize(buf_start + nb_samples * nb_channels);
            float *dst = m_audio_buf.data() + buf_start;

            uint8_t *dst_ptr = (uint8_t *)dst;
            int converted = m_ff.swr_convert(m_swr, &dst_ptr, nb_samples,
                (const uint8_t **)m_audio_frame->data, nb_samples);
            if (converted < 0) {
                m_audio_buf.resize(buf_start);
                set_error("swr_convert failed.");
                return false;
            }
            if (converted < nb_samples)
                m_audio_buf.resize(buf_start + converted * nb_channels);

            m_ff.av_frame_unref(m_audio_frame);
            return true;
        }

        if (ret != AVERROR(EAGAIN)) {
            if (ret == AVERROR_EOF) m_eof = true;
            return false;
        }

        while (true) {
            ret = m_ff.av_read_frame(m_fmt_ctx, m_packet);
            if (ret < 0) {
                m_ff.avcodec_send_packet(m_audio_codec_ctx, nullptr);
                return false;
            }
            if (m_packet->stream_index == m_audio_stream_idx) {
                int send_ret = m_ff.avcodec_send_packet(m_audio_codec_ctx, m_packet);
                m_ff.av_packet_unref(m_packet);
                if (send_ret < 0 && send_ret != AVERROR(EAGAIN)) {
                    set_ff_error("avcodec_send_packet (audio)", send_ret);
                    return false;
                }
                break;
            }
            m_ff.av_packet_unref(m_packet);
        }
    }
}

int AvbBackendFFmpeg::read_audio_f32(float *dst_interleaved, int frames) {
    if (!m_audio_codec_ctx || m_eof) return 0;

    int nb_channels    = m_audio_codec_ctx->ch_layout.nb_channels;
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

avb_result AvbBackendFFmpeg::read_video_frame(avb_video_frame &out_frame) {
    if (!m_video_codec_ctx) return AVB_ERROR_STREAM_NOT_FOUND;

    while (true) {
        int ret = m_ff.avcodec_receive_frame(m_video_codec_ctx, m_video_frame);
        if (ret == 0) {
            int w = m_video_frame->width;
            int h = m_video_frame->height;
            AVPixelFormat src_fmt = (AVPixelFormat)m_video_frame->format;

            // Rebuild swscale context if source properties changed
            if (!m_sws || m_sws_src_w != w || m_sws_src_h != h || m_sws_src_fmt != src_fmt) {
                if (m_sws) m_ff.sws_freeContext(m_sws);
                m_sws = m_ff.sws_getContext(
                    w, h, src_fmt,
                    w, h, AV_PIX_FMT_BGRA,
                    SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (!m_sws) {
                    m_ff.av_frame_unref(m_video_frame);
                    set_error("sws_getContext failed (unsupported pixel format?).");
                    return AVB_ERROR_DECODE_FAILED;
                }
                m_sws_src_w   = w;
                m_sws_src_h   = h;
                m_sws_src_fmt = src_fmt;
            }

            // Convert to BGRA into m_video_out_buf
            int stride = w * 4;
            m_video_out_buf.resize(stride * h);
            uint8_t *dst_data[1]   = { m_video_out_buf.data() };
            int      dst_stride[1] = { stride };
            m_ff.sws_scale(m_sws,
                (const uint8_t *const *)m_video_frame->data, m_video_frame->linesize,
                0, h, dst_data, dst_stride);

            // Compute PTS
            AVStream *st = m_fmt_ctx->streams[m_video_stream_idx];
            double pts_sec = 0.0;
            if (m_video_frame->pts != AV_NOPTS_VALUE && st->time_base.den != 0)
                pts_sec = (double)m_video_frame->pts * st->time_base.num / st->time_base.den;

            m_ff.av_frame_unref(m_video_frame);

            out_frame.width     = w;
            out_frame.height    = h;
            out_frame.format    = AVB_PIXEL_FORMAT_BGRA8;
            out_frame.stride    = stride;
            out_frame.pts_sec   = pts_sec;
            out_frame.data      = m_video_out_buf.data();
            out_frame.data_size = (int)m_video_out_buf.size();
            return AVB_OK;
        }

        if (ret != AVERROR(EAGAIN)) {
            if (ret == AVERROR_EOF) return AVB_ERROR_EOF;
            set_ff_error("avcodec_receive_frame (video)", ret);
            return AVB_ERROR_DECODE_FAILED;
        }

        // Feed more packets
        while (true) {
            ret = m_ff.av_read_frame(m_fmt_ctx, m_packet);
            if (ret < 0) {
                m_ff.avcodec_send_packet(m_video_codec_ctx, nullptr);
                return AVB_ERROR_EOF;
            }
            if (m_packet->stream_index == m_video_stream_idx) {
                int send_ret = m_ff.avcodec_send_packet(m_video_codec_ctx, m_packet);
                m_ff.av_packet_unref(m_packet);
                if (send_ret < 0 && send_ret != AVERROR(EAGAIN)) {
                    set_ff_error("avcodec_send_packet (video)", send_ret);
                    return AVB_ERROR_DECODE_FAILED;
                }
                break;
            }
            m_ff.av_packet_unref(m_packet);
        }
    }
}

void AvbBackendFFmpeg::release_video_frame(avb_video_frame &frame) {
    // Output buffer is owned by the backend and reused on the next read_video_frame call.
    // Just zero the caller's frame struct so they can't accidentally use stale pointers.
    memset(&frame, 0, sizeof(frame));
}
