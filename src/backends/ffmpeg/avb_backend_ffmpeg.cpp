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
        } else if (sidx == m_video_stream_idx && m_video_codec_ctx) {
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
    m_seek_target = -1.0;
    m_audio_stream_idx = -1;
    m_video_stream_idx = -1;
}

avb_result AvbBackendFFmpeg::open_file(const char *path, const avb_decode_options &options) {
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
            default:
                m_video_format = AVB_PIXEL_FORMAT_BGRA8; m_dst_av_fmt = AV_PIX_FMT_BGRA; break;
        }
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

    clear_packet_queues();
    m_audio_buf.clear();
    m_audio_buf_pos = 0;
    m_eof = false;
    m_seek_target = seconds;
    return AVB_OK;
}

bool AvbBackendFFmpeg::fill_audio_buffer() {
    if (!m_audio_codec_ctx) return false;

    while (true) {
        int ret = m_ff.avcodec_receive_frame(m_audio_codec_ctx, m_audio_frame);
        if (ret == 0) {
            int nb_channels = m_out_channels;
            int in_samples  = m_audio_frame->nb_samples;

            // Output sample count after resampling can differ from the input;
            // size for buffered delay + this frame, rounded up.
            int in_rate = m_audio_codec_ctx->sample_rate;
            int64_t delay = m_ff.swr_get_delay(m_swr, in_rate);
            int out_capacity =
                (int)(((int64_t)in_samples + delay) * m_out_sample_rate / in_rate) + 1;

            int buf_start = (int)m_audio_buf.size();
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

            int w = m_video_frame->width;
            int h = m_video_frame->height;
            AVPixelFormat src_fmt = (AVPixelFormat)m_video_frame->format;

            // Rebuild swscale context if source properties changed
            if (!m_sws || m_sws_src_w != w || m_sws_src_h != h || m_sws_src_fmt != src_fmt) {
                if (m_sws) m_ff.sws_freeContext(m_sws);
                m_sws = m_ff.sws_getContext(
                    w, h, src_fmt,
                    w, h, m_dst_av_fmt,
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
                (const uint8_t *const *)m_video_frame->data, m_video_frame->linesize,
                0, h, dst_data, dst_stride);

            m_ff.av_frame_unref(m_video_frame);

            out_frame = {};
            out_frame.width       = w;
            out_frame.height      = h;
            out_frame.format      = m_video_format;
            out_frame.pts_sec     = frame_pts;
            out_frame.plane_count = plane_count;
            for (int p = 0; p < plane_count; ++p) {
                out_frame.plane_data[p]   = dst_data[p];
                out_frame.plane_stride[p] = dst_stride[p];
            }
            out_frame.data      = out_frame.plane_data[0];
            out_frame.stride    = out_frame.plane_stride[0];
            out_frame.data_size = (int)m_video_out_buf.size();
            return AVB_OK;
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
    // Output buffer is owned by the backend and reused on the next read_video_frame call.
    // Just zero the caller's frame struct so they can't accidentally use stale pointers.
    memset(&frame, 0, sizeof(frame));
}
