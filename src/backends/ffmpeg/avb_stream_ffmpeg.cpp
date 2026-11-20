#include "avb_stream_ffmpeg.hpp"
#include "avb_pixel_convert.hpp"
#include "avb_plane_layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

AVCodecID video_codec_id(avb_video_codec codec) {
    switch (codec) {
        case AVB_VIDEO_CODEC_H264: return AV_CODEC_ID_H264;
        case AVB_VIDEO_CODEC_HEVC: return AV_CODEC_ID_HEVC;
        case AVB_VIDEO_CODEC_VP8:  return AV_CODEC_ID_VP8;
        case AVB_VIDEO_CODEC_VP9:  return AV_CODEC_ID_VP9;
        case AVB_VIDEO_CODEC_AV1:  return AV_CODEC_ID_AV1;
        default:                   return AV_CODEC_ID_NONE;
    }
}

AVPixelFormat av_pixel_format(avb_pixel_format format) {
    switch (format) {
        case AVB_PIXEL_FORMAT_RGBA8: return AV_PIX_FMT_RGBA;
        case AVB_PIXEL_FORMAT_NV12:  return AV_PIX_FMT_NV12;
        case AVB_PIXEL_FORMAT_I420:  return AV_PIX_FMT_YUV420P;
        default:                     return AV_PIX_FMT_BGRA;
    }
}

struct HwEncoder {
    const char *name;
    avb_hardware_device device;
};

// Hardware encoders that accept system-memory frames, in platform order.
std::vector<HwEncoder> hardware_encoders(avb_video_codec codec, avb_hardware_device wanted) {
    const char *h264[] = {"h264_amf", "h264_nvenc", "h264_qsv", "h264_videotoolbox", "h264_v4l2m2m"};
    const char *hevc[] = {"hevc_amf", "hevc_nvenc", "hevc_qsv", "hevc_videotoolbox", "hevc_v4l2m2m"};
    const char *av1[]  = {"av1_amf", "av1_nvenc", "av1_qsv", nullptr, nullptr};
    const char *vp9[]  = {nullptr, nullptr, "vp9_qsv", nullptr, nullptr};
    const char *vp8[]  = {nullptr, nullptr, nullptr, nullptr, "vp8_v4l2m2m"};
    const avb_hardware_device devices[] = {
        AVB_HW_DEVICE_AMF, AVB_HW_DEVICE_CUDA, AVB_HW_DEVICE_QSV,
        AVB_HW_DEVICE_VIDEOTOOLBOX, AVB_HW_DEVICE_V4L2,
    };
    const char **names = codec == AVB_VIDEO_CODEC_HEVC ? hevc :
                         codec == AVB_VIDEO_CODEC_AV1 ? av1 :
                         codec == AVB_VIDEO_CODEC_VP9 ? vp9 :
                         codec == AVB_VIDEO_CODEC_VP8 ? vp8 : h264;
    std::vector<HwEncoder> out;
    for (int i = 0; i < 5; ++i) {
        if (!names[i]) continue;
        const avb_hardware_device device = devices[i];
        const bool match = wanted == AVB_HW_DEVICE_AUTO || wanted == device ||
                           (wanted == AVB_HW_DEVICE_D3D11VA && device == AVB_HW_DEVICE_AMF);
        if (match) out.push_back({names[i], device});
    }
#if defined(__linux__)
    // NVENC first on Linux desktops; AMF there is a niche.
    std::stable_partition(out.begin(), out.end(),
                          [](const HwEncoder &e) { return e.device == AVB_HW_DEVICE_CUDA; });
#endif
    return out;
}

const char *profile_name(const char *encoder, avb_video_profile profile) {
    const bool amf = std::strstr(encoder, "_amf") != nullptr;
    const bool vt = std::strstr(encoder, "videotoolbox") != nullptr;
    switch (profile) {
        case AVB_VIDEO_PROFILE_H264_CONSTRAINED_BASELINE:
            return amf || vt ? "constrained_baseline" : "baseline";
        case AVB_VIDEO_PROFILE_H264_BASELINE: return "baseline";
        case AVB_VIDEO_PROFILE_H264_MAIN: return "main";
        case AVB_VIDEO_PROFILE_H264_HIGH: return "high";
        case AVB_VIDEO_PROFILE_HEVC_MAIN: return "main";
        default: return nullptr;
    }
}

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
AVSampleFormat pick_sample_format(const AvbFFmpegFuncs &ff, const AVCodec *codec) {
    static const AVSampleFormat preferred[] = {
        AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_S16P,
    };
    const AVSampleFormat *formats = nullptr;
    int count = 0;
#if LIBAVCODEC_VERSION_MAJOR >= 61
    const void *configs = nullptr;
    if (ff.avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                        &configs, &count) >= 0 && configs) {
        formats = static_cast<const AVSampleFormat *>(configs);
    }
#else
    (void)ff;
    formats = codec->sample_fmts;
    if (formats) while (formats[count] != AV_SAMPLE_FMT_NONE) ++count;
#endif
    if (!formats || count <= 0) return AV_SAMPLE_FMT_FLTP;
    for (AVSampleFormat want : preferred) {
        for (int i = 0; i < count; ++i)
            if (formats[i] == want) return want;
    }
    return formats[0];
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

} // namespace

/* ================================================================ encoder */

AvbStreamEncoderFFmpeg::AvbStreamEncoderFFmpeg() {
    char err[512];
    m_loaded = avb_ffmpeg_load(m_ff, err, sizeof(err));
    if (!m_loaded) m_last_error = err;
}

AvbStreamEncoderFFmpeg::~AvbStreamEncoderFFmpeg() { close_internal(); }

const char *AvbStreamEncoderFFmpeg::get_last_error() const {
    return m_last_error.empty() ? nullptr : m_last_error.c_str();
}

void AvbStreamEncoderFFmpeg::set_ff_error(const char *prefix, int err) {
    char buf[256];
    m_ff.av_strerror(err, buf, sizeof(buf));
    m_last_error = std::string(prefix) + ": " + buf;
}

void AvbStreamEncoderFFmpeg::close_internal() {
    if (!m_loaded) return;
    if (m_sws) { m_ff.sws_freeContext(m_sws); m_sws = nullptr; }
    if (m_swr) m_ff.swr_free(&m_swr);
    if (m_frame) m_ff.av_frame_free(&m_frame);
    if (m_pkt) m_ff.av_packet_free(&m_pkt);
    if (m_ctx) m_ff.avcodec_free_context(&m_ctx);
    m_queue.clear();
    m_fifo.clear();
}

avb_result AvbStreamEncoderFFmpeg::open_video(const AvbStreamVideoEncodeConfig &config) {
    if (!m_loaded) return AVB_ERROR_BACKEND_NOT_AVAILABLE;
    close_internal();
    const avb_video_stream_encode_params &p = config.params;
    const avb_video_encode_params &v = p.video;
    m_is_video = true;
    m_video = config;
    if (v.input_memory != AVB_VIDEO_MEMORY_CPU) {
        m_last_error = "The FFmpeg stream encoder takes CPU frames.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    m_src_fmt = av_pixel_format(v.input_format);
    const AVCodecID id = video_codec_id(v.codec);
    const AVRational fps = m_ff.av_d2q(v.frame_rate, 1000000);
    const bool bt709 = avb::detail::use_bt709(v.width, v.height);

    struct Candidate {
        const AVCodec *codec;
        avb_hardware_device device;
    };
    std::vector<Candidate> candidates;
    if (v.hardware_policy != AVB_HARDWARE_DISABLED) {
        for (const HwEncoder &hw : hardware_encoders(v.codec, v.hardware_device)) {
            if (const AVCodec *c = m_ff.avcodec_find_encoder_by_name(hw.name))
                candidates.push_back({c, hw.device});
        }
    }
    if (v.hardware_policy != AVB_HARDWARE_REQUIRE) {
        const char *preferred = v.codec == AVB_VIDEO_CODEC_H264 ? "libx264" :
                                v.codec == AVB_VIDEO_CODEC_HEVC ? "libx265" :
                                v.codec == AVB_VIDEO_CODEC_VP8 ? "libvpx" :
                                v.codec == AVB_VIDEO_CODEC_VP9 ? "libvpx-vp9" : "libsvtav1";
        const AVCodec *c = m_ff.avcodec_find_encoder_by_name(preferred);
        if (!c) c = m_ff.avcodec_find_encoder(id);
        if (c) candidates.push_back({c, AVB_HW_DEVICE_AUTO});
    }
    if (candidates.empty()) {
        m_last_error = v.hardware_policy == AVB_HARDWARE_REQUIRE
            ? "No FFmpeg hardware encoder that takes system-memory frames is available for this codec."
            : "This FFmpeg build has no encoder for the requested codec.";
        return AVB_ERROR_OPEN_FAILED;
    }

    for (const Candidate &candidate : candidates) {
        const AVCodec *codec = candidate.codec;
        const char *name = codec->name ? codec->name : "";
        const bool hw = candidate.device != AVB_HW_DEVICE_AUTO;
        const bool nv12 = hw && std::strstr(name, "v4l2m2m") == nullptr;
        m_ctx = m_ff.avcodec_alloc_context3(codec);
        if (!m_ctx) {
            m_last_error = "avcodec_alloc_context3 (video) failed.";
            return AVB_ERROR_OPEN_FAILED;
        }
        m_ctx->width = v.width;
        m_ctx->height = v.height;
        m_ctx->pix_fmt = nv12 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
        m_ctx->time_base = AVRational{1, AVB_STREAM_VIDEO_TIMESCALE};
        m_ctx->framerate = fps;
        m_ctx->bit_rate = v.bitrate;
        m_ctx->color_range = AVCOL_RANGE_MPEG;
        m_ctx->colorspace = bt709 ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
        m_ctx->color_primaries = bt709 ? AVCOL_PRI_BT709 : AVCOL_PRI_SMPTE170M;
        m_ctx->color_trc = bt709 ? AVCOL_TRC_BT709 : AVCOL_TRC_SMPTE170M;
        if (p.gop_size > 0) m_ctx->gop_size = p.gop_size;
        if (p.max_b_frames >= 0) m_ctx->max_b_frames = p.max_b_frames;
        if (p.low_latency) m_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        if (p.rate_control == AVB_RATE_CONTROL_CBR) {
            m_ctx->rc_max_rate = v.bitrate;
            m_ctx->rc_min_rate = v.bitrate;
            m_ctx->rc_buffer_size = v.bitrate;
        } else if (p.max_bitrate > 0) {
            m_ctx->rc_max_rate = p.max_bitrate;
            m_ctx->rc_buffer_size = p.max_bitrate * 2;
        }

        AVDictionary *opts = nullptr;
        if (const char *profile = profile_name(name, p.profile))
            m_ff.av_dict_set(&opts, "profile", profile, 0);
        if (std::strcmp(name, "libx264") == 0 || std::strcmp(name, "libx265") == 0) {
            m_ff.av_dict_set(&opts, "preset", "veryfast", 0);
            if (p.low_latency) m_ff.av_dict_set(&opts, "tune", "zerolatency", 0);
            m_ff.av_dict_set(&opts, "forced-idr", "1", 0);
            if (p.rate_control == AVB_RATE_CONTROL_CBR && std::strcmp(name, "libx264") == 0)
                m_ff.av_dict_set(&opts, "nal-hrd", "cbr", 0);
        } else if (std::strstr(name, "nvenc")) {
            m_ff.av_dict_set(&opts, "preset", "p4", 0);
            if (p.low_latency) {
                m_ff.av_dict_set(&opts, "tune", "ll", 0);
                m_ff.av_dict_set(&opts, "zerolatency", "1", 0);
                m_ff.av_dict_set(&opts, "delay", "0", 0);
            }
            m_ff.av_dict_set(&opts, "forced-idr", "1", 0);
            if (p.rate_control == AVB_RATE_CONTROL_CBR) m_ff.av_dict_set(&opts, "rc", "cbr", 0);
        } else if (std::strstr(name, "_amf")) {
            m_ff.av_dict_set(&opts, "usage", p.low_latency ? "lowlatency" : "transcoding", 0);
            m_ff.av_dict_set(&opts, "forced_idr", "1", 0);
            if (p.rate_control == AVB_RATE_CONTROL_CBR) m_ff.av_dict_set(&opts, "rc", "cbr", 0);
        } else if (std::strstr(name, "_qsv")) {
            m_ff.av_dict_set(&opts, "forced_idr", "1", 0);
            if (p.low_latency) m_ff.av_dict_set(&opts, "async_depth", "1", 0);
        } else if (std::strstr(name, "videotoolbox")) {
            if (p.low_latency) m_ff.av_dict_set(&opts, "realtime", "1", 0);
        } else if (std::strcmp(name, "libvpx") == 0 || std::strcmp(name, "libvpx-vp9") == 0) {
            m_ff.av_dict_set(&opts, "deadline", "realtime", 0);
            m_ff.av_dict_set(&opts, "cpu-used", "8", 0);
            if (p.low_latency) m_ff.av_dict_set(&opts, "lag-in-frames", "0", 0);
        } else if (std::strcmp(name, "libsvtav1") == 0) {
            m_ff.av_dict_set(&opts, "preset", "10", 0);
        } else if (std::strcmp(name, "libaom-av1") == 0) {
            m_ff.av_dict_set(&opts, "cpu-used", "8", 0);
            m_ff.av_dict_set(&opts, "usage", "realtime", 0);
        }
        const int ret = m_ff.avcodec_open2(m_ctx, codec, &opts);
        m_ff.av_dict_free(&opts);
        if (ret >= 0) {
            m_output.hardware_device = candidate.device;
            break;
        }
        set_ff_error((std::string("avcodec_open2 (") + name + ")").c_str(), ret);
        m_ff.avcodec_free_context(&m_ctx);
    }
    if (!m_ctx) return AVB_ERROR_OPEN_FAILED;
    m_last_error.clear();

    m_frame = m_ff.av_frame_alloc();
    m_pkt = m_ff.av_packet_alloc();
    if (!m_frame || !m_pkt) {
        m_last_error = "Allocating FFmpeg frame/packet failed.";
        return AVB_ERROR_OPEN_FAILED;
    }
    m_frame->format = m_ctx->pix_fmt;
    m_frame->width = v.width;
    m_frame->height = v.height;
    m_frame->color_range = m_ctx->color_range;
    m_frame->colorspace = m_ctx->colorspace;
    int ret = m_ff.av_frame_get_buffer(m_frame, 0);
    if (ret < 0) {
        set_ff_error("av_frame_get_buffer (video)", ret);
        return AVB_ERROR_OPEN_FAILED;
    }
    m_sws = m_ff.sws_getContext(v.width, v.height, m_src_fmt, v.width, v.height, m_ctx->pix_fmt,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_sws) {
        m_last_error = "sws_getContext (stream encode) failed.";
        return AVB_ERROR_OPEN_FAILED;
    }
    if (m_src_fmt == AV_PIX_FMT_BGRA || m_src_fmt == AV_PIX_FMT_RGBA) {
        const int *coefficients = m_ff.sws_getCoefficients(bt709 ? SWS_CS_ITU709 : SWS_CS_ITU601);
        m_ff.sws_setColorspaceDetails(m_sws, coefficients, 1, coefficients, 0, 0, 1 << 16, 1 << 16);
    }
    m_output.length_prefixed = false;
    return AVB_OK;
}

avb_result AvbStreamEncoderFFmpeg::open_audio(const AvbStreamAudioEncodeConfig &config) {
    if (!m_loaded) return AVB_ERROR_BACKEND_NOT_AVAILABLE;
    close_internal();
    const avb_audio_encode_params &a = config.params.audio;
    m_is_video = false;
    m_audio = config;

    const AVCodec *codec = nullptr;
    if (a.codec == AVB_AUDIO_CODEC_OPUS) {
        codec = m_ff.avcodec_find_encoder_by_name("libopus");
        if (!codec) codec = m_ff.avcodec_find_encoder(AV_CODEC_ID_OPUS);
    } else {
        codec = m_ff.avcodec_find_encoder_by_name("libfdk_aac");
        if (!codec) codec = m_ff.avcodec_find_encoder(AV_CODEC_ID_AAC);
    }
    if (!codec) {
        m_last_error = "This FFmpeg build has no encoder for the requested audio codec.";
        return AVB_ERROR_OPEN_FAILED;
    }
    m_ctx = m_ff.avcodec_alloc_context3(codec);
    if (!m_ctx) {
        m_last_error = "avcodec_alloc_context3 (audio) failed.";
        return AVB_ERROR_OPEN_FAILED;
    }
    m_ctx->sample_rate = a.sample_rate;
    m_ff.av_channel_layout_default(&m_ctx->ch_layout, a.channels);
    m_ctx->sample_fmt = pick_sample_format(m_ff, codec);
    m_ctx->bit_rate = a.bitrate;
    m_ctx->time_base = AVRational{1, a.sample_rate};
    m_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (codec->name && std::strcmp(codec->name, "opus") == 0)
        m_ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    int ret = m_ff.avcodec_open2(m_ctx, codec, nullptr);
    if (ret < 0) {
        set_ff_error("avcodec_open2 (audio)", ret);
        return AVB_ERROR_OPEN_FAILED;
    }
    m_frame_size = m_ctx->frame_size > 0 ? m_ctx->frame_size : 1024;
    m_pad_last = (codec->capabilities &
                  (AV_CODEC_CAP_SMALL_LAST_FRAME | AV_CODEC_CAP_VARIABLE_FRAME_SIZE)) == 0;
    if (m_ctx->extradata && m_ctx->extradata_size > 0)
        m_output.extradata.assign(m_ctx->extradata, m_ctx->extradata + m_ctx->extradata_size);
    m_output.encoder_delay = m_ctx->initial_padding;

    AVChannelLayout in_layout;
    m_ff.av_channel_layout_default(&in_layout, a.channels);
    ret = m_ff.swr_alloc_set_opts2(&m_swr, &m_ctx->ch_layout, m_ctx->sample_fmt, a.sample_rate,
                                   &in_layout, AV_SAMPLE_FMT_FLT, a.sample_rate, 0, nullptr);
    m_ff.av_channel_layout_uninit(&in_layout);
    if (ret < 0 || !m_swr || (ret = m_ff.swr_init(m_swr)) < 0) {
        set_ff_error("swr_init (stream encode)", ret);
        return AVB_ERROR_OPEN_FAILED;
    }
    m_frame = m_ff.av_frame_alloc();
    m_pkt = m_ff.av_packet_alloc();
    if (!m_frame || !m_pkt) {
        m_last_error = "Allocating FFmpeg frame/packet failed.";
        return AVB_ERROR_OPEN_FAILED;
    }
    m_frame->format = m_ctx->sample_fmt;
    m_frame->sample_rate = a.sample_rate;
    m_frame->nb_samples = m_frame_size;
    m_ff.av_channel_layout_copy(&m_frame->ch_layout, &m_ctx->ch_layout);
    ret = m_ff.av_frame_get_buffer(m_frame, 0);
    if (ret < 0) {
        set_ff_error("av_frame_get_buffer (audio)", ret);
        return AVB_ERROR_OPEN_FAILED;
    }
    return AVB_OK;
}

int AvbStreamEncoderFFmpeg::pull_packets() {
    for (;;) {
        const int ret = m_ff.avcodec_receive_packet(m_ctx, m_pkt);
        if (ret < 0) return ret;
        AvbStreamEncodedPacket packet;
        packet.data.assign(m_pkt->data, m_pkt->data + m_pkt->size);
        packet.pts = m_pkt->pts != AV_NOPTS_VALUE ? m_pkt->pts : -1;
        packet.dts = m_pkt->dts != AV_NOPTS_VALUE ? m_pkt->dts : -1;
        packet.duration = m_pkt->duration > 0 ? m_pkt->duration : -1;
        packet.keyframe = (m_pkt->flags & AV_PKT_FLAG_KEY) != 0;
        m_ff.av_packet_unref(m_pkt);
        m_queue.push_back(std::move(packet));
    }
}

avb_result AvbStreamEncoderFFmpeg::send_video(const avb_video_frame &frame, double pts_sec,
                                              bool force_keyframe) {
    if (!m_ctx || !m_is_video) return AVB_ERROR_INVALID_ARGUMENT;
    int ret = m_ff.av_frame_make_writable(m_frame);
    if (ret < 0) {
        set_ff_error("av_frame_make_writable (video)", ret);
        return AVB_ERROR_ENCODE_FAILED;
    }
    const uint8_t *src[4] = {};
    int lines[4] = {};
    for (int i = 0; i < frame.plane_count && i < 4; ++i) {
        src[i] = frame.plane_data[i];
        lines[i] = frame.plane_stride[i];
    }
    if (m_ff.sws_scale(m_sws, src, lines, 0, frame.height, m_frame->data, m_frame->linesize) !=
        frame.height) {
        m_last_error = "sws_scale (stream encode) did not produce a complete frame.";
        return AVB_ERROR_ENCODE_FAILED;
    }
    m_frame->pts = std::llround(pts_sec * AVB_STREAM_VIDEO_TIMESCALE);
    m_frame->pict_type = force_keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    ret = m_ff.avcodec_send_frame(m_ctx, m_frame);
    if (ret == AVERROR(EAGAIN)) {
        pull_packets();
        ret = m_ff.avcodec_send_frame(m_ctx, m_frame);
    }
    if (ret < 0) {
        set_ff_error("avcodec_send_frame (video)", ret);
        return AVB_ERROR_ENCODE_FAILED;
    }
    ret = pull_packets();
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        set_ff_error("avcodec_receive_packet (video)", ret);
        return AVB_ERROR_ENCODE_FAILED;
    }
    return AVB_OK;
}

avb_result AvbStreamEncoderFFmpeg::encode_audio_chunk(int frames) {
    int ret = m_ff.av_frame_make_writable(m_frame);
    if (ret < 0) {
        set_ff_error("av_frame_make_writable (audio)", ret);
        return AVB_ERROR_ENCODE_FAILED;
    }
    const uint8_t *in[1] = {reinterpret_cast<const uint8_t *>(m_fifo.data())};
    const int got = m_ff.swr_convert(m_swr, m_frame->data, frames, in, frames);
    if (got < 0) {
        m_last_error = "swr_convert (stream encode) failed.";
        return AVB_ERROR_ENCODE_FAILED;
    }
    m_frame->nb_samples = got;
    m_frame->pts = m_audio_pts;
    m_audio_pts += got;
    const int channels = m_audio.params.audio.channels;
    m_fifo.erase(m_fifo.begin(), m_fifo.begin() + (size_t)frames * channels);
    ret = m_ff.avcodec_send_frame(m_ctx, m_frame);
    if (ret == AVERROR(EAGAIN)) {
        pull_packets();
        ret = m_ff.avcodec_send_frame(m_ctx, m_frame);
    }
    if (ret < 0) {
        set_ff_error("avcodec_send_frame (audio)", ret);
        return AVB_ERROR_ENCODE_FAILED;
    }
    ret = pull_packets();
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        set_ff_error("avcodec_receive_packet (audio)", ret);
        return AVB_ERROR_ENCODE_FAILED;
    }
    return AVB_OK;
}

avb_result AvbStreamEncoderFFmpeg::send_audio(const float *interleaved, int frames) {
    if (!m_ctx || m_is_video) return AVB_ERROR_INVALID_ARGUMENT;
    const int channels = m_audio.params.audio.channels;
    m_fifo.insert(m_fifo.end(), interleaved, interleaved + (size_t)frames * channels);
    while (m_fifo.size() >= (size_t)m_frame_size * channels) {
        const avb_result res = encode_audio_chunk(m_frame_size);
        if (res != AVB_OK) return res;
    }
    return AVB_OK;
}

avb_result AvbStreamEncoderFFmpeg::receive(AvbStreamEncodedPacket &out) {
    if (!m_ctx) return AVB_ERROR_INVALID_ARGUMENT;
    if (m_queue.empty() && m_flushing && !m_eof) {
        const int ret = pull_packets();
        if (ret == AVERROR_EOF) m_eof = true;
        else if (ret < 0 && ret != AVERROR(EAGAIN)) {
            set_ff_error("avcodec_receive_packet (flush)", ret);
            return AVB_ERROR_ENCODE_FAILED;
        }
    }
    if (m_queue.empty()) return m_eof ? AVB_ERROR_EOF : AVB_ERROR_AGAIN;
    out = std::move(m_queue.front());
    m_queue.pop_front();
    return AVB_OK;
}

avb_result AvbStreamEncoderFFmpeg::set_bitrate(int bitrate) {
    if (!m_ctx) return AVB_ERROR_INVALID_ARGUMENT;
    if (!m_is_video) {
        m_last_error = "The FFmpeg audio encoders cannot change their bitrate while encoding.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    // libx264 and NVENC reconfigure themselves when they see the new value
    // on the next frame.
    m_ctx->bit_rate = bitrate;
    if (m_video.params.rate_control == AVB_RATE_CONTROL_CBR) {
        m_ctx->rc_max_rate = bitrate;
        m_ctx->rc_min_rate = bitrate;
        m_ctx->rc_buffer_size = bitrate;
    }
    return AVB_OK;
}

avb_result AvbStreamEncoderFFmpeg::flush() {
    if (!m_ctx) return AVB_ERROR_INVALID_ARGUMENT;
    if (!m_is_video) {
        const int channels = m_audio.params.audio.channels;
        const int left = (int)(m_fifo.size() / channels);
        if (left > 0) {
            if (m_pad_last) m_fifo.resize((size_t)m_frame_size * channels, 0.0f);
            const avb_result res = encode_audio_chunk(m_pad_last ? m_frame_size : left);
            if (res != AVB_OK) return res;
        }
    }
    m_flushing = true;
    const int ret = m_ff.avcodec_send_frame(m_ctx, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) {
        set_ff_error("avcodec_send_frame (flush)", ret);
        return AVB_ERROR_ENCODE_FAILED;
    }
    return AVB_OK;
}

/* ================================================================ decoder */

AvbStreamDecoderFFmpeg::AvbStreamDecoderFFmpeg() {
    char err[512];
    m_loaded = avb_ffmpeg_load(m_ff, err, sizeof(err));
    if (!m_loaded) m_last_error = err;
}

AvbStreamDecoderFFmpeg::~AvbStreamDecoderFFmpeg() { close_internal(); }

const char *AvbStreamDecoderFFmpeg::get_last_error() const {
    return m_last_error.empty() ? nullptr : m_last_error.c_str();
}

void AvbStreamDecoderFFmpeg::set_ff_error(const char *prefix, int err) {
    char buf[256];
    m_ff.av_strerror(err, buf, sizeof(buf));
    m_last_error = std::string(prefix) + ": " + buf;
}

void AvbStreamDecoderFFmpeg::clear_frames() {
    for (AVFrame *frame : m_pending) m_ff.av_frame_free(&frame);
    m_pending.clear();
}

void AvbStreamDecoderFFmpeg::close_internal() {
    if (!m_loaded) return;
    clear_frames();
    if (m_sws) { m_ff.sws_freeContext(m_sws); m_sws = nullptr; }
    if (m_swr) m_ff.swr_free(&m_swr);
    if (m_frame) m_ff.av_frame_free(&m_frame);
    if (m_pkt) m_ff.av_packet_free(&m_pkt);
    if (m_ctx) m_ff.avcodec_free_context(&m_ctx);
}

void AvbStreamDecoderFFmpeg::audio_format(int &sample_rate, int &channels) const {
    sample_rate = m_sample_rate;
    channels = m_channels;
}

avb_result AvbStreamDecoderFFmpeg::open(const AvbStreamDecodeConfig &config) {
    if (!m_loaded) return AVB_ERROR_BACKEND_NOT_AVAILABLE;
    close_internal();
    m_config = config;
    const avb_stream_decode_params &p = config.params;
    const AVCodec *codec = nullptr;
    if (p.is_video) {
        if (p.video_memory != AVB_VIDEO_MEMORY_CPU) {
            m_last_error = "The FFmpeg stream decoder outputs CPU frames.";
            return AVB_ERROR_INVALID_ARGUMENT;
        }
        if (p.hardware_policy == AVB_HARDWARE_REQUIRE) {
            m_last_error = "The FFmpeg stream decoder decodes in software.";
            return AVB_ERROR_OPEN_FAILED;
        }
        codec = m_ff.avcodec_find_decoder(video_codec_id(p.video_codec));
        m_timescale = AVB_STREAM_VIDEO_TIMESCALE;
    } else {
        codec = m_ff.avcodec_find_decoder(p.audio_codec == AVB_AUDIO_CODEC_OPUS ? AV_CODEC_ID_OPUS
                                                                                 : AV_CODEC_ID_AAC);
        m_timescale = p.sample_rate > 0 ? p.sample_rate : 48000;
    }
    if (!codec) {
        m_last_error = "This FFmpeg build has no decoder for the requested codec.";
        return AVB_ERROR_OPEN_FAILED;
    }
    m_ctx = m_ff.avcodec_alloc_context3(codec);
    if (!m_ctx) {
        m_last_error = "avcodec_alloc_context3 (decode) failed.";
        return AVB_ERROR_OPEN_FAILED;
    }
    m_ctx->pkt_timebase = AVRational{1, m_timescale};
    if (p.is_video) {
        // In-band parameter sets with Annex-B framing: no avcC extradata, or
        // the decoder would expect length prefixes.
        if (p.low_latency) {
            m_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
            m_ctx->thread_type = FF_THREAD_SLICE;
        }
    } else {
        m_ctx->sample_rate = p.sample_rate;
        m_ff.av_channel_layout_default(&m_ctx->ch_layout, p.channels > 0 ? p.channels : 2);
        if (!config.extradata.empty()) {
            m_ctx->extradata = (uint8_t *)m_ff.av_malloc(config.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!m_ctx->extradata) {
                m_last_error = "av_malloc (extradata) failed.";
                return AVB_ERROR_OPEN_FAILED;
            }
            std::memcpy(m_ctx->extradata, config.extradata.data(), config.extradata.size());
            std::memset(m_ctx->extradata + config.extradata.size(), 0, AV_INPUT_BUFFER_PADDING_SIZE);
            m_ctx->extradata_size = (int)config.extradata.size();
        }
    }
    const int ret = m_ff.avcodec_open2(m_ctx, codec, nullptr);
    if (ret < 0) {
        set_ff_error("avcodec_open2 (decode)", ret);
        return AVB_ERROR_OPEN_FAILED;
    }
    m_frame = m_ff.av_frame_alloc();
    m_pkt = m_ff.av_packet_alloc();
    if (!m_frame || !m_pkt) {
        m_last_error = "Allocating FFmpeg frame/packet failed.";
        return AVB_ERROR_OPEN_FAILED;
    }
    return AVB_OK;
}

avb_result AvbStreamDecoderFFmpeg::send(const AvbStreamDecodeInput &input) {
    if (!m_ctx) return AVB_ERROR_INVALID_ARGUMENT;
    int ret;
    if (!input.data) {
        ret = m_ff.avcodec_send_packet(m_ctx, nullptr);
        if (ret < 0 && ret != AVERROR_EOF) {
            set_ff_error("avcodec_send_packet (drain)", ret);
            return AVB_ERROR_DECODE_FAILED;
        }
        return AVB_OK;
    }
    ret = m_ff.av_new_packet(m_pkt, (int)input.size);
    if (ret < 0) {
        set_ff_error("av_new_packet", ret);
        return AVB_ERROR_DECODE_FAILED;
    }
    std::memcpy(m_pkt->data, input.data, input.size);
    m_pkt->pts = input.pts_sec >= 0.0 ? std::llround(input.pts_sec * m_timescale) : AV_NOPTS_VALUE;
    m_pkt->dts = AV_NOPTS_VALUE;
    if (input.keyframe) m_pkt->flags |= AV_PKT_FLAG_KEY;
    ret = m_ff.avcodec_send_packet(m_ctx, m_pkt);
    while (ret == AVERROR(EAGAIN)) {
        // The decoder holds output we have not taken yet: queue it.
        const int got = m_ff.avcodec_receive_frame(m_ctx, m_frame);
        if (got < 0) break;
        AVFrame *copy = m_ff.av_frame_alloc();
        if (!copy || m_ff.av_frame_ref(copy, m_frame) < 0) {
            if (copy) m_ff.av_frame_free(&copy);
            m_ff.av_frame_unref(m_frame);
            break;
        }
        m_ff.av_frame_unref(m_frame);
        m_pending.push_back(copy);
        ret = m_ff.avcodec_send_packet(m_ctx, m_pkt);
    }
    m_ff.av_packet_unref(m_pkt);
    if (ret < 0) {
        // A corrupt access unit is reported, not fatal: the caller resets
        // and waits for the next keyframe.
        set_ff_error("avcodec_send_packet", ret);
        return AVB_ERROR_DECODE_FAILED;
    }
    return AVB_OK;
}

int AvbStreamDecoderFFmpeg::next_frame() {
    m_ff.av_frame_unref(m_frame);
    if (!m_pending.empty()) {
        AVFrame *queued = m_pending.front();
        m_pending.pop_front();
        const int ret = m_ff.av_frame_ref(m_frame, queued);
        m_ff.av_frame_free(&queued);
        return ret;
    }
    return m_ff.avcodec_receive_frame(m_ctx, m_frame);
}

avb_result AvbStreamDecoderFFmpeg::receive_video(avb_video_frame &out) {
    if (!m_ctx || !m_config.params.is_video) return AVB_ERROR_INVALID_ARGUMENT;
    const int ret = next_frame();
    if (ret == AVERROR(EAGAIN)) return AVB_ERROR_AGAIN;
    if (ret == AVERROR_EOF) return AVB_ERROR_EOF;
    if (ret < 0) {
        set_ff_error("avcodec_receive_frame (video)", ret);
        return AVB_ERROR_DECODE_FAILED;
    }
    const int w = m_frame->width, h = m_frame->height;
    const AVPixelFormat fmt = (AVPixelFormat)m_frame->format;
    const avb_pixel_format out_fmt = m_config.params.video_format;
    if (!m_sws || m_sws_w != w || m_sws_h != h || m_sws_fmt != fmt ||
        m_sws_space != m_frame->colorspace || m_sws_range != m_frame->color_range) {
        if (m_sws) m_ff.sws_freeContext(m_sws);
        m_sws = m_ff.sws_getContext(w, h, fmt, w, h, av_pixel_format(out_fmt), SWS_BILINEAR,
                                    nullptr, nullptr, nullptr);
        if (!m_sws) {
            m_last_error = "sws_getContext (stream decode) failed.";
            return AVB_ERROR_DECODE_FAILED;
        }
        int space = m_frame->colorspace;
        if (space == AVCOL_SPC_UNSPECIFIED || space == AVCOL_SPC_RESERVED || space == AVCOL_SPC_RGB)
            space = avb::detail::use_bt709(w, h) ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
        const int *coefficients = m_ff.sws_getCoefficients(space);
        const int full = m_frame->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
        const bool rgb = out_fmt == AVB_PIXEL_FORMAT_BGRA8 || out_fmt == AVB_PIXEL_FORMAT_RGBA8;
        m_ff.sws_setColorspaceDetails(m_sws, coefficients, full, coefficients, rgb ? 1 : full,
                                      0, 1 << 16, 1 << 16);
        m_sws_w = w;
        m_sws_h = h;
        m_sws_fmt = fmt;
        m_sws_space = m_frame->colorspace;
        m_sws_range = m_frame->color_range;
    }
    const AvbPlaneLayout layout = avb_plane_layout(out_fmt, w, h, 1);
    m_video_buf.resize(layout.total);
    uint8_t *dst[4] = {};
    int dst_stride[4] = {};
    for (int i = 0; i < layout.plane_count; ++i) {
        dst[i] = m_video_buf.data() + layout.offset[i];
        dst_stride[i] = layout.stride[i];
    }
    m_ff.sws_scale(m_sws, (const uint8_t *const *)m_frame->data, m_frame->linesize, 0, h, dst,
                   dst_stride);
    const int64_t ts = m_frame->best_effort_timestamp != AV_NOPTS_VALUE
        ? m_frame->best_effort_timestamp : m_frame->pts;
    avb::detail::init_cpu_frame(out, w, h, out_fmt,
                                ts != AV_NOPTS_VALUE ? (double)ts / m_timescale : -1.0);
    out.plane_count = layout.plane_count;
    for (int i = 0; i < layout.plane_count; ++i) {
        out.plane_data[i] = dst[i];
        out.plane_stride[i] = dst_stride[i];
        out.plane_offset[i] = (int)layout.offset[i];
    }
    out.data = out.plane_data[0];
    out.stride = out.plane_stride[0];
    out.data_size = (int)m_video_buf.size();
    if (out_fmt == AVB_PIXEL_FORMAT_NV12 || out_fmt == AVB_PIXEL_FORMAT_I420) {
        out.color_range = m_frame->color_range == AVCOL_RANGE_JPEG ? AVB_COLOR_RANGE_FULL
                                                                   : AVB_COLOR_RANGE_LIMITED;
        out.color_matrix = m_frame->colorspace == AVCOL_SPC_BT709 ? AVB_COLOR_MATRIX_BT709 :
                           m_frame->colorspace == AVCOL_SPC_BT2020_NCL ? AVB_COLOR_MATRIX_BT2020_NCL :
                           m_frame->colorspace == AVCOL_SPC_UNSPECIFIED ? AVB_COLOR_MATRIX_UNKNOWN :
                                                                          AVB_COLOR_MATRIX_BT601;
    }
    m_ff.av_frame_unref(m_frame);
    return AVB_OK;
}

avb_result AvbStreamDecoderFFmpeg::receive_audio(std::vector<float> &out, int &frames,
                                                 double &pts_sec) {
    frames = 0;
    pts_sec = -1.0;
    if (!m_ctx || m_config.params.is_video) return AVB_ERROR_INVALID_ARGUMENT;
    const int ret = next_frame();
    if (ret == AVERROR(EAGAIN)) return AVB_ERROR_AGAIN;
    if (ret == AVERROR_EOF) return AVB_ERROR_EOF;
    if (ret < 0) {
        set_ff_error("avcodec_receive_frame (audio)", ret);
        return AVB_ERROR_DECODE_FAILED;
    }
    const int rate = m_frame->sample_rate;
    const int channels = m_frame->ch_layout.nb_channels;
    if (!m_swr || m_swr_rate != rate || m_swr_channels != channels || m_swr_fmt != m_frame->format) {
        if (m_swr) m_ff.swr_free(&m_swr);
        AVChannelLayout out_layout;
        m_ff.av_channel_layout_default(&out_layout, channels);
        int r = m_ff.swr_alloc_set_opts2(&m_swr, &out_layout, AV_SAMPLE_FMT_FLT, rate,
                                         &m_frame->ch_layout, (AVSampleFormat)m_frame->format, rate,
                                         0, nullptr);
        m_ff.av_channel_layout_uninit(&out_layout);
        if (r < 0 || !m_swr || (r = m_ff.swr_init(m_swr)) < 0) {
            set_ff_error("swr_init (stream decode)", r);
            m_ff.av_frame_unref(m_frame);
            return AVB_ERROR_DECODE_FAILED;
        }
        m_swr_rate = rate;
        m_swr_channels = channels;
        m_swr_fmt = m_frame->format;
    }
    m_sample_rate = rate;
    m_channels = channels;
    out.resize((size_t)m_frame->nb_samples * channels);
    uint8_t *dst[1] = {reinterpret_cast<uint8_t *>(out.data())};
    const int got = m_ff.swr_convert(m_swr, dst, m_frame->nb_samples,
                                     (const uint8_t **)m_frame->extended_data, m_frame->nb_samples);
    const int64_t ts = m_frame->best_effort_timestamp != AV_NOPTS_VALUE
        ? m_frame->best_effort_timestamp : m_frame->pts;
    m_ff.av_frame_unref(m_frame);
    if (got < 0) {
        m_last_error = "swr_convert (stream decode) failed.";
        return AVB_ERROR_DECODE_FAILED;
    }
    frames = got;
    out.resize((size_t)got * channels);
    if (ts != AV_NOPTS_VALUE) pts_sec = (double)ts / m_timescale;
    return AVB_OK;
}

avb_result AvbStreamDecoderFFmpeg::reset() {
    if (!m_ctx) return AVB_ERROR_INVALID_ARGUMENT;
    clear_frames();
    m_ff.avcodec_flush_buffers(m_ctx);
    return AVB_OK;
}
