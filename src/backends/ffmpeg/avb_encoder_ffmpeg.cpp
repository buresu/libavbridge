#include "avb_encoder_ffmpeg.hpp"
#include "../../avb_video_codec_registry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

static AVCodecID avb_ff_video_codec_id(avb_codec codec) {
    switch (codec) {
        case AVB_CODEC_H264: return AV_CODEC_ID_H264;
        case AVB_CODEC_HEVC: return AV_CODEC_ID_HEVC;
        case AVB_CODEC_VP9:  return AV_CODEC_ID_VP9;
        case AVB_CODEC_HAP:  return AV_CODEC_ID_HAP;
        default:             return AV_CODEC_ID_NONE;
    }
}

AvbEncoderFFmpeg::AvbEncoderFFmpeg() {
    char err_buf[512];
    m_libs_loaded = avb_ffmpeg_load(m_ff, err_buf, sizeof(err_buf));
    if (!m_libs_loaded) m_last_error = err_buf;
}

AvbEncoderFFmpeg::~AvbEncoderFFmpeg() {
    close_internal();
}

const char *AvbEncoderFFmpeg::get_last_error() const {
    return m_last_error.empty() ? nullptr : m_last_error.c_str();
}

void AvbEncoderFFmpeg::set_error(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    m_last_error = buf;
}

void AvbEncoderFFmpeg::set_ff_error(const char *prefix, int errnum) {
    char errbuf[256];
    m_ff.av_strerror(errnum, errbuf, sizeof(errbuf));
    set_error("%s: %s", prefix, errbuf);
}

void AvbEncoderFFmpeg::close_internal() {
    if (!m_libs_loaded) return;

    if (m_sws)    { m_ff.sws_freeContext(m_sws); m_sws = nullptr; }
    if (m_swr)    { m_ff.swr_free(&m_swr); m_swr = nullptr; }
    if (m_vframe) { m_ff.av_frame_free(&m_vframe); m_vframe = nullptr; }
    if (m_aframe) { m_ff.av_frame_free(&m_aframe); m_aframe = nullptr; }
    if (m_packet) { m_ff.av_packet_free(&m_packet); m_packet = nullptr; }
    if (m_custom_video_encoder) {
        if (m_custom_video_encoder->close && m_custom_video_ctx)
            m_custom_video_encoder->close(m_custom_video_ctx);
        m_custom_video_encoder = nullptr;
        m_custom_video_ctx = nullptr;
        m_custom_video_stream = {};
    }
    if (m_venc)   { m_ff.avcodec_free_context(&m_venc); m_venc = nullptr; }
    if (m_aenc)   { m_ff.avcodec_free_context(&m_aenc); m_aenc = nullptr; }
    if (m_fmt_ctx) {
        if (m_fmt_ctx->pb && !(m_fmt_ctx->oformat->flags & AVFMT_NOFILE))
            m_ff.avio_closep(&m_fmt_ctx->pb);
        m_ff.avformat_free_context(m_fmt_ctx);
        m_fmt_ctx = nullptr;
    }
    m_audio_fifo.clear();
}

avb_result AvbEncoderFFmpeg::open(const char *path, const avb_encode_options &options) {
    if (!m_libs_loaded) return AVB_ERROR_BACKEND_NOT_AVAILABLE;
    close_internal();

    if (!options.video.enable && !options.audio.enable) {
        set_error("Encoder requires at least one of video/audio enabled.");
        return AVB_ERROR_INVALID_ARGUMENT;
    }

    int ret = m_ff.avformat_alloc_output_context2(&m_fmt_ctx, nullptr, nullptr, path);
    if (ret < 0 || !m_fmt_ctx) {
        set_ff_error("avformat_alloc_output_context2 failed", ret);
        return AVB_ERROR_OPEN_FAILED;
    }
    bool global_header = (m_fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) != 0;

    // --- Video ---
    if (options.video.enable) {
        if (options.video.width <= 0 || options.video.height <= 0) {
            set_error("Video width/height must be positive.");
            return AVB_ERROR_INVALID_ARGUMENT;
        }
        switch (options.video.input_format) {
            case AVB_PIXEL_FORMAT_RGBA8: m_input_format = AVB_PIXEL_FORMAT_RGBA8; m_src_av_fmt = AV_PIX_FMT_RGBA; break;
            case AVB_PIXEL_FORMAT_NV12:  m_input_format = AVB_PIXEL_FORMAT_NV12;  m_src_av_fmt = AV_PIX_FMT_NV12; break;
            case AVB_PIXEL_FORMAT_I420:  m_input_format = AVB_PIXEL_FORMAT_I420;  m_src_av_fmt = AV_PIX_FMT_YUV420P; break;
            case AVB_PIXEL_FORMAT_BC1_RGBA:
            case AVB_PIXEL_FORMAT_BC3_RGBA:
            case AVB_PIXEL_FORMAT_BC4_R:
            case AVB_PIXEL_FORMAT_BC5_RG:
            case AVB_PIXEL_FORMAT_BC7_RGBA:
                m_input_format = options.video.input_format;
                break;
            case AVB_PIXEL_FORMAT_BGRA8:
            case AVB_PIXEL_FORMAT_UNKNOWN:
            default:                     m_input_format = AVB_PIXEL_FORMAT_BGRA8; m_src_av_fmt = AV_PIX_FMT_BGRA; break;
        }
        m_width      = options.video.width;
        m_height     = options.video.height;
        m_frame_rate = options.video.frame_rate > 0 ? options.video.frame_rate : 30.0;
        m_fps_den    = std::max(1L, std::lround(m_frame_rate));

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
                set_error("Custom video encoder '%s' failed to open.",
                          plugin->name ? plugin->name : "(unnamed)");
                return cres;
            }

            m_vstream = m_ff.avformat_new_stream(m_fmt_ctx, nullptr);
            if (!m_vstream) { set_error("avformat_new_stream (custom video) failed."); return AVB_ERROR_OPEN_FAILED; }
            m_vstream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
            m_vstream->codecpar->codec_id = avb_ff_video_codec_id(stream.codec);
            if (m_vstream->codecpar->codec_id == AV_CODEC_ID_NONE)
                m_vstream->codecpar->codec_id = avb_ff_video_codec_id(options.video.codec);
            m_vstream->codecpar->codec_tag = stream.codec_tag;
            m_vstream->codecpar->width = m_width;
            m_vstream->codecpar->height = m_height;
            if (stream.extradata && stream.extradata_size > 0) {
                m_vstream->codecpar->extradata =
                    (uint8_t *)m_ff.av_malloc((size_t)stream.extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
                if (!m_vstream->codecpar->extradata) {
                    set_error("av_malloc (custom video extradata) failed.");
                    return AVB_ERROR_OPEN_FAILED;
                }
                memcpy(m_vstream->codecpar->extradata, stream.extradata, stream.extradata_size);
                memset(m_vstream->codecpar->extradata + stream.extradata_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
                m_vstream->codecpar->extradata_size = stream.extradata_size;
            }
            int tb_den = stream.time_base_den > 0 ? stream.time_base_den : m_fps_den;
            int tb_num = stream.time_base_num > 0 ? stream.time_base_num : 1;
            m_vstream->time_base = AVRational{tb_num, tb_den};
            m_custom_video_encoder = plugin;
            m_custom_video_ctx = ctx;
            m_custom_video_stream = stream;
            m_custom_video = true;
            m_has_video = true;
        } else {

        AVCodecID vid;
        const char *vname;
        switch (options.video.codec) {
            case AVB_CODEC_AUTO:
            case AVB_CODEC_H264: vid = AV_CODEC_ID_H264; vname = "H.264"; break;
            case AVB_CODEC_HEVC: vid = AV_CODEC_ID_HEVC; vname = "HEVC";  break;
            case AVB_CODEC_VP9:  vid = AV_CODEC_ID_VP9;  vname = "VP9";   break;
            default:
                set_error("Invalid video codec (use AUTO/H264/HEVC/VP9).");
                return AVB_ERROR_INVALID_ARGUMENT;
        }
        const AVCodec *vcodec = m_ff.avcodec_find_encoder(vid);
        if (!vcodec) { set_error("No %s encoder available in this FFmpeg build.", vname); return AVB_ERROR_OPEN_FAILED; }

        m_vstream = m_ff.avformat_new_stream(m_fmt_ctx, nullptr);
        if (!m_vstream) { set_error("avformat_new_stream (video) failed."); return AVB_ERROR_OPEN_FAILED; }

        m_venc = m_ff.avcodec_alloc_context3(vcodec);
        if (!m_venc) { set_error("avcodec_alloc_context3 (video) failed."); return AVB_ERROR_OPEN_FAILED; }
        m_venc->width     = m_width;
        m_venc->height    = m_height;
        m_venc->pix_fmt   = AV_PIX_FMT_YUV420P;
        m_venc->time_base = AVRational{1, m_fps_den};
        m_venc->framerate = AVRational{m_fps_den, 1};
        if (options.video.bitrate > 0) m_venc->bit_rate = options.video.bitrate;
        if (global_header) m_venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        // Speed knobs for the common encoders. avcodec_open2 silently ignores
        // options a given encoder doesn't recognise, so it is safe to set the
        // x264/x265 "preset" and the libvpx "deadline"/"cpu-used" together.
        AVDictionary *vopts = nullptr;
        m_ff.av_dict_set(&vopts, "preset",   "veryfast", 0);
        m_ff.av_dict_set(&vopts, "deadline", "realtime", 0);
        m_ff.av_dict_set(&vopts, "cpu-used", "8",        0);
        ret = m_ff.avcodec_open2(m_venc, vcodec, &vopts);
        m_ff.av_dict_free(&vopts);
        if (ret < 0) { set_ff_error("avcodec_open2 (video)", ret); return AVB_ERROR_OPEN_FAILED; }

        ret = m_ff.avcodec_parameters_from_context(m_vstream->codecpar, m_venc);
        if (ret < 0) { set_ff_error("avcodec_parameters_from_context (video)", ret); return AVB_ERROR_OPEN_FAILED; }
        m_vstream->time_base = m_venc->time_base;

        m_vframe = m_ff.av_frame_alloc();
        if (!m_vframe) { set_error("av_frame_alloc (video) failed."); return AVB_ERROR_OPEN_FAILED; }
        m_vframe->format = AV_PIX_FMT_YUV420P;
        m_vframe->width  = m_width;
        m_vframe->height = m_height;
        ret = m_ff.av_frame_get_buffer(m_vframe, 0);
        if (ret < 0) { set_ff_error("av_frame_get_buffer (video)", ret); return AVB_ERROR_OPEN_FAILED; }

        m_has_video = true;
        }
    }

    // --- Audio ---
    if (options.audio.enable) {
        if (options.audio.sample_rate <= 0 || options.audio.channels <= 0) {
            set_error("Audio sample_rate/channels must be positive.");
            return AVB_ERROR_INVALID_ARGUMENT;
        }
        m_sample_rate = options.audio.sample_rate;
        m_channels    = options.audio.channels;

        AVCodecID aid;
        AVSampleFormat asfmt;
        const char *aname;
        switch (options.audio.codec) {
            case AVB_CODEC_AUTO:
            case AVB_CODEC_AAC:
                aid = AV_CODEC_ID_AAC; asfmt = AV_SAMPLE_FMT_FLTP; aname = "AAC"; break;
            case AVB_CODEC_OPUS:
                aid = AV_CODEC_ID_OPUS; asfmt = AV_SAMPLE_FMT_FLT; aname = "Opus";
                // libopus only accepts a fixed set of input rates.
                switch (m_sample_rate) {
                    case 8000: case 12000: case 16000: case 24000: case 48000: break;
                    default:
                        set_error("Opus requires sample_rate of 8000/12000/16000/24000/48000.");
                        return AVB_ERROR_INVALID_ARGUMENT;
                }
                break;
            default:
                set_error("Invalid audio codec (use AUTO/AAC/OPUS).");
                return AVB_ERROR_INVALID_ARGUMENT;
        }
        const AVCodec *acodec = m_ff.avcodec_find_encoder(aid);
        if (!acodec) { set_error("No %s encoder available in this FFmpeg build.", aname); return AVB_ERROR_OPEN_FAILED; }

        m_astream = m_ff.avformat_new_stream(m_fmt_ctx, nullptr);
        if (!m_astream) { set_error("avformat_new_stream (audio) failed."); return AVB_ERROR_OPEN_FAILED; }

        m_aenc = m_ff.avcodec_alloc_context3(acodec);
        if (!m_aenc) { set_error("avcodec_alloc_context3 (audio) failed."); return AVB_ERROR_OPEN_FAILED; }
        m_aenc->sample_rate = m_sample_rate;
        m_ff.av_channel_layout_default(&m_aenc->ch_layout, m_channels);
        m_aenc->sample_fmt  = asfmt;
        m_asfmt             = asfmt;
        m_aenc->bit_rate    = options.audio.bitrate > 0 ? options.audio.bitrate : 128000;
        m_aenc->time_base   = AVRational{1, m_sample_rate};
        if (global_header) m_aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        ret = m_ff.avcodec_open2(m_aenc, acodec, nullptr);
        if (ret < 0) { set_ff_error("avcodec_open2 (audio)", ret); return AVB_ERROR_OPEN_FAILED; }
        m_frame_size = m_aenc->frame_size > 0 ? m_aenc->frame_size : 1024;

        ret = m_ff.avcodec_parameters_from_context(m_astream->codecpar, m_aenc);
        if (ret < 0) { set_ff_error("avcodec_parameters_from_context (audio)", ret); return AVB_ERROR_OPEN_FAILED; }
        m_astream->time_base = AVRational{1, m_sample_rate};

        // Resampler: interleaved float (API input) -> planar float (encoder).
        AVChannelLayout in_layout;
        m_ff.av_channel_layout_default(&in_layout, m_channels);
        m_swr = m_ff.swr_alloc();
        ret = m_ff.swr_alloc_set_opts2(&m_swr,
            &m_aenc->ch_layout, m_asfmt,           m_sample_rate,
            &in_layout,         AV_SAMPLE_FMT_FLT,  m_sample_rate,
            0, nullptr);
        m_ff.av_channel_layout_uninit(&in_layout);
        if (ret < 0 || !m_swr) { set_ff_error("swr_alloc_set_opts2 (audio)", ret); return AVB_ERROR_OPEN_FAILED; }
        ret = m_ff.swr_init(m_swr);
        if (ret < 0) { set_ff_error("swr_init (audio)", ret); return AVB_ERROR_OPEN_FAILED; }

        m_aframe = m_ff.av_frame_alloc();
        if (!m_aframe) { set_error("av_frame_alloc (audio) failed."); return AVB_ERROR_OPEN_FAILED; }
        m_aframe->format      = m_asfmt;
        m_aframe->sample_rate = m_sample_rate;
        m_aframe->nb_samples  = m_frame_size;
        m_ff.av_channel_layout_copy(&m_aframe->ch_layout, &m_aenc->ch_layout);
        ret = m_ff.av_frame_get_buffer(m_aframe, 0);
        if (ret < 0) { set_ff_error("av_frame_get_buffer (audio)", ret); return AVB_ERROR_OPEN_FAILED; }

        m_has_audio = true;
    }

    // Open the output file and write the container header.
    if (!(m_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = m_ff.avio_open(&m_fmt_ctx->pb, path, AVIO_FLAG_WRITE);
        if (ret < 0) { set_ff_error("avio_open failed", ret); return AVB_ERROR_OPEN_FAILED; }
    }
    ret = m_ff.avformat_write_header(m_fmt_ctx, nullptr);
    if (ret < 0) { set_ff_error("avformat_write_header failed", ret); return AVB_ERROR_OPEN_FAILED; }

    m_packet = m_ff.av_packet_alloc();
    if (!m_packet) { set_error("av_packet_alloc failed."); return AVB_ERROR_OPEN_FAILED; }

    return AVB_OK;
}

avb_result AvbEncoderFFmpeg::encode_and_mux(AVCodecContext *enc, AVStream *stream, AVFrame *frame) {
    int ret = m_ff.avcodec_send_frame(enc, frame);
    if (ret < 0 && ret != AVERROR_EOF) {
        set_ff_error("avcodec_send_frame", ret);
        return AVB_ERROR_ENCODE_FAILED;
    }
    while (true) {
        ret = m_ff.avcodec_receive_packet(enc, m_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
            set_ff_error("avcodec_receive_packet", ret);
            return AVB_ERROR_ENCODE_FAILED;
        }
        m_packet->stream_index = stream->index;
        m_ff.av_packet_rescale_ts(m_packet, enc->time_base, stream->time_base);
        ret = m_ff.av_interleaved_write_frame(m_fmt_ctx, m_packet);
        m_ff.av_packet_unref(m_packet); // already unref'd by write_frame; harmless
        if (ret < 0) {
            set_ff_error("av_interleaved_write_frame", ret);
            return AVB_ERROR_ENCODE_FAILED;
        }
    }
    return AVB_OK;
}

avb_result AvbEncoderFFmpeg::write_video(const avb_video_frame &frame, double pts_sec) {
    if (!m_has_video) return AVB_ERROR_INVALID_ARGUMENT;
    if (m_custom_video) {
        avb_encoded_packet packet{};
        avb_result res = m_custom_video_encoder->encode_frame(
            m_custom_video_ctx, &frame, pts_sec, &packet);
        if (res != AVB_OK) return res;
        res = write_custom_video_packet(packet);
        if (m_custom_video_encoder->release_packet)
            m_custom_video_encoder->release_packet(m_custom_video_ctx, &packet);
        if (res == AVB_OK) m_video_index++;
        return res;
    }
    if (frame.format != m_input_format) {
        set_error("Frame pixel format does not match configured input_format.");
        return AVB_ERROR_INVALID_ARGUMENT;
    }

    double pts = pts_sec >= 0.0      ? pts_sec
               : frame.pts_sec >= 0.0 ? frame.pts_sec
               : (double)m_video_index / m_frame_rate;

    if (!m_sws) {
        m_sws = m_ff.sws_getContext(m_width, m_height, m_src_av_fmt,
                                    m_width, m_height, AV_PIX_FMT_YUV420P,
                                    SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_sws) { set_error("sws_getContext (encode) failed."); return AVB_ERROR_ENCODE_FAILED; }
    }

    int ret = m_ff.av_frame_make_writable(m_vframe);
    if (ret < 0) { set_ff_error("av_frame_make_writable (video)", ret); return AVB_ERROR_ENCODE_FAILED; }

    const uint8_t *src_data[4] = {nullptr, nullptr, nullptr, nullptr};
    int            src_lines[4] = {0, 0, 0, 0};
    for (int p = 0; p < frame.plane_count && p < 4; ++p) {
        src_data[p]  = frame.plane_data[p];
        src_lines[p] = frame.plane_stride[p];
    }
    m_ff.sws_scale(m_sws, src_data, src_lines, 0, m_height,
                   m_vframe->data, m_vframe->linesize);

    m_vframe->pts = std::llround(pts * m_fps_den);
    m_video_index++;

    return encode_and_mux(m_venc, m_vstream, m_vframe);
}

avb_result AvbEncoderFFmpeg::write_custom_video_packet(avb_encoded_packet &packet) {
    if (!packet.data || packet.size <= 0) return AVB_ERROR_INVALID_ARGUMENT;

    AVPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    int ret = m_ff.av_new_packet(&pkt, packet.size);
    if (ret < 0) { set_ff_error("av_new_packet (custom video)", ret); return AVB_ERROR_ENCODE_FAILED; }
    memcpy(pkt.data, packet.data, packet.size);
    pkt.stream_index = m_vstream->index;
    pkt.pts = packet.pts >= 0
        ? packet.pts
        : (int64_t)std::llround((packet.pts_sec >= 0.0
            ? packet.pts_sec
            : (double)m_video_index / m_frame_rate) / av_q2d(m_vstream->time_base));
    pkt.dts = packet.dts >= 0 ? packet.dts : pkt.pts;
    pkt.duration = packet.duration >= 0
        ? packet.duration
        : (int64_t)std::llround((packet.duration_sec > 0.0
            ? packet.duration_sec
            : 1.0 / m_frame_rate) / av_q2d(m_vstream->time_base));
    if (packet.keyframe) pkt.flags |= AV_PKT_FLAG_KEY;

    ret = m_ff.av_interleaved_write_frame(m_fmt_ctx, &pkt);
    m_ff.av_packet_unref(&pkt);
    if (ret < 0) {
        set_ff_error("av_interleaved_write_frame (custom video)", ret);
        return AVB_ERROR_ENCODE_FAILED;
    }
    return AVB_OK;
}

avb_result AvbEncoderFFmpeg::encode_audio_frame(int nb_samples) {
    int ret = m_ff.av_frame_make_writable(m_aframe);
    if (ret < 0) { set_ff_error("av_frame_make_writable (audio)", ret); return AVB_ERROR_ENCODE_FAILED; }

    const uint8_t *in[1] = { (const uint8_t *)m_audio_fifo.data() };
    int got = m_ff.swr_convert(m_swr, m_aframe->data, nb_samples, in, nb_samples);
    if (got < 0) { set_error("swr_convert (audio) failed."); return AVB_ERROR_ENCODE_FAILED; }

    m_aframe->nb_samples = got;
    m_aframe->pts = m_audio_pts;
    m_audio_pts += got;

    avb_result r = encode_and_mux(m_aenc, m_astream, m_aframe);

    // Drop the consumed interleaved samples from the FIFO front.
    m_audio_fifo.erase(m_audio_fifo.begin(),
                       m_audio_fifo.begin() + (size_t)nb_samples * m_channels);
    return r;
}

avb_result AvbEncoderFFmpeg::write_audio_f32(const float *src_interleaved, int frames) {
    if (!m_has_audio) return AVB_ERROR_INVALID_ARGUMENT;

    m_audio_fifo.insert(m_audio_fifo.end(), src_interleaved,
                        src_interleaved + (size_t)frames * m_channels);

    size_t chunk = (size_t)m_frame_size * m_channels;
    while (m_audio_fifo.size() >= chunk) {
        avb_result r = encode_audio_frame(m_frame_size);
        if (r != AVB_OK) return r;
    }
    return AVB_OK;
}

avb_result AvbEncoderFFmpeg::finish() {
    if (!m_fmt_ctx) return AVB_ERROR_INVALID_ARGUMENT;
    if (m_finished) return AVB_OK;

    // Encode any partial audio frame still in the FIFO, then flush both encoders.
    if (m_has_audio) {
        int leftover = (int)(m_audio_fifo.size() / m_channels);
        if (leftover > 0) {
            avb_result r = encode_audio_frame(leftover);
            if (r != AVB_OK) return r;
        }
        avb_result r = encode_and_mux(m_aenc, m_astream, nullptr);
        if (r != AVB_OK) return r;
    }
    if (m_has_video) {
        if (m_custom_video) {
            while (m_custom_video_encoder->flush) {
                avb_encoded_packet packet{};
                avb_result r = m_custom_video_encoder->flush(m_custom_video_ctx, &packet);
                if (r == AVB_ERROR_EOF || r == AVB_ERROR_AGAIN) break;
                if (r != AVB_OK) return r;
                r = write_custom_video_packet(packet);
                if (m_custom_video_encoder->release_packet)
                    m_custom_video_encoder->release_packet(m_custom_video_ctx, &packet);
                if (r != AVB_OK) return r;
            }
        } else {
            avb_result r = encode_and_mux(m_venc, m_vstream, nullptr);
            if (r != AVB_OK) return r;
        }
    }

    int ret = m_ff.av_write_trailer(m_fmt_ctx);
    if (ret < 0) { set_ff_error("av_write_trailer failed", ret); return AVB_ERROR_ENCODE_FAILED; }

    if (m_fmt_ctx->pb && !(m_fmt_ctx->oformat->flags & AVFMT_NOFILE))
        m_ff.avio_closep(&m_fmt_ctx->pb);

    m_finished = true;
    return AVB_OK;
}
