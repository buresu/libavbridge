#include "avbridge.h"
#include "avb_backend.hpp"
#include "avb_bitstream.hpp"
#include "avb_frame_validation.hpp"
#include "avb_stream_impl.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using avb::detail::NalRef;
using avb::detail::ParamSetKind;
using avb::detail::ParamSets;

namespace {

constexpr int k_default_video_bitrate = 2000000;
constexpr int k_default_audio_bitrate = 128000;

bool valid_profile(avb_video_codec codec, avb_video_profile profile) {
    switch (profile) {
        case AVB_VIDEO_PROFILE_AUTO:
            return true;
        case AVB_VIDEO_PROFILE_H264_CONSTRAINED_BASELINE:
        case AVB_VIDEO_PROFILE_H264_BASELINE:
        case AVB_VIDEO_PROFILE_H264_MAIN:
        case AVB_VIDEO_PROFILE_H264_HIGH:
            return codec == AVB_VIDEO_CODEC_H264;
        case AVB_VIDEO_PROFILE_HEVC_MAIN:
            return codec == AVB_VIDEO_CODEC_HEVC;
        default:
            return false;
    }
}

bool packet_has_kind(avb_video_codec codec, const std::vector<NalRef> &nals, ParamSetKind kind) {
    for (const NalRef &nal : nals) {
        if (avb::detail::param_set_kind(codec, avb::detail::nal_type(codec, nal.data, nal.size)) == kind)
            return true;
    }
    return false;
}

} // namespace

struct avb_stream_encoder {
    std::unique_ptr<AvbStreamEncoderImpl> impl;
    std::string last_error;
    avb_backend backend = AVB_BACKEND_AUTO;
    bool is_video = false;
    bool opened = false;
    bool flushing = false;
    bool eof = false;

    AvbStreamVideoEncodeConfig video;
    AvbStreamAudioEncodeConfig audio;

    bool keyframe_requested = false;
    long video_index = 0;
    double last_video_pts = -std::numeric_limits<double>::infinity();
    bool audio_started = false;
    int64_t audio_start = 0;

    ParamSets params;
    std::vector<uint8_t> extradata;
    AvbStreamEncodedPacket packet;
    std::vector<uint8_t> out_data;
    std::vector<NalRef> nals;

    void set_error(const char *message) { last_error = message ? message : ""; }
    void capture_error() {
        const char *err = impl ? impl->get_last_error() : nullptr;
        if (err && err[0]) last_error = err;
    }

    avb_video_codec video_codec() const { return video.params.video.codec; }
    bool h26x() const { return is_video && avb::detail::is_h26x(video_codec()); }

    void refresh_extradata() {
        if (is_video) {
            if (!h26x()) {
                if (extradata.empty()) extradata = impl->output().extradata;
                return;
            }
            if (!params.complete()) {
                const auto &seed = impl->output().extradata;
                if (!seed.empty()) params.load_extradata(seed.data(), seed.size());
            }
            if (params.complete()) extradata = params.configuration_record();
            return;
        }
        const auto &backend_extradata = impl->output().extradata;
        if (!backend_extradata.empty()) {
            extradata = backend_extradata;
            return;
        }
        const avb_audio_encode_params &a = audio.params.audio;
        if (a.codec == AVB_AUDIO_CODEC_AAC)
            extradata = avb::detail::aac_audio_specific_config(a.sample_rate, a.channels);
        else if (a.codec == AVB_AUDIO_CODEC_OPUS)
            extradata = avb::detail::opus_head(a.channels, impl->output().encoder_delay, a.sample_rate);
    }
};

struct avb_stream_decoder {
    std::unique_ptr<AvbStreamDecoderImpl> impl;
    std::string last_error;
    avb_backend backend = AVB_BACKEND_AUTO;
    avb_stream_decode_params params{};
    bool opened = false;
    bool draining = false;

    bool h26x = false;
    ParamSets ps;
    int nal_length_size = 4;
    bool waiting_keyframe = true;
    bool config_pending = false;
    std::vector<uint8_t> config;      // avcC/hvcC
    std::vector<uint8_t> audio_extra; // ASC / OpusHead
    std::vector<uint8_t> input;
    std::vector<NalRef> nals;

    std::vector<float> audio;
    int audio_frames = 0;
    int audio_pos = 0;
    double audio_pts = -1.0;

    int width = 0;
    int height = 0;

    void set_error(const char *message) { last_error = message ? message : ""; }
    void capture_error() {
        const char *err = impl ? impl->get_last_error() : nullptr;
        if (err && err[0]) last_error = err;
    }
};

extern "C" {

avb_video_stream_encode_params avb_video_stream_encode_params_default(void) {
    avb_video_stream_encode_params p{};
    p.backend = AVB_BACKEND_AUTO;
    p.video = avb_encode_options_default().video;
    p.video.enable = 1;
    p.profile = AVB_VIDEO_PROFILE_AUTO;
    p.gop_size = 0;
    p.max_b_frames = -1;
    p.low_latency = 0;
    p.rate_control = AVB_RATE_CONTROL_AUTO;
    p.max_bitrate = 0;
    p.repeat_parameter_sets = 1;
    p.bitstream = AVB_BITSTREAM_AUTO;
    return p;
}

avb_audio_stream_encode_params avb_audio_stream_encode_params_default(void) {
    avb_audio_stream_encode_params p{};
    p.backend = AVB_BACKEND_AUTO;
    p.audio = avb_encode_options_default().audio;
    p.audio.enable = 1;
    return p;
}

avb_stream_decode_params avb_stream_decode_params_default(void) {
    avb_stream_decode_params p{};
    p.backend = AVB_BACKEND_AUTO;
    p.is_video = 1;
    p.video_codec = AVB_VIDEO_CODEC_AUTO;
    p.audio_codec = AVB_AUDIO_CODEC_AUTO;
    p.bitstream = AVB_BITSTREAM_AUTO;
    p.video_format = AVB_PIXEL_FORMAT_UNKNOWN;
    p.video_memory = AVB_VIDEO_MEMORY_CPU;
    p.video_external_type = AVB_VIDEO_EXTERNAL_NONE;
    p.hardware_policy = AVB_HARDWARE_DISABLED;
    p.hardware_device = AVB_HW_DEVICE_AUTO;
    return p;
}

/* ---------------------------------------------------------------- encoder */

static avb_stream_encoder *new_encoder(avb_backend backend, avb_result &result) {
    auto *enc = new avb_stream_encoder();
    enc->backend = avb::detail::resolve_backend(backend);
    enc->impl = avb_create_stream_encoder_backend(backend);
    result = AVB_OK;
    if (!enc->impl) {
        enc->set_error("Requested backend is not available for elementary-stream encoding on this platform.");
        result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
    }
    return enc;
}

avb_result avb_stream_encoder_open_video(avb_stream_encoder **out,
                                         const avb_video_stream_encode_params *params) {
    if (!out || !params) return AVB_ERROR_INVALID_ARGUMENT;
    avb_result result;
    avb_stream_encoder *enc = new_encoder(params->backend, result);
    *out = enc;
    enc->is_video = true;
    if (result != AVB_OK) return result;

    avb_video_stream_encode_params p = *params;
    avb_video_encode_params &v = p.video;
    v.enable = 1;
    if (v.codec == AVB_VIDEO_CODEC_AUTO) v.codec = AVB_VIDEO_CODEC_H264;
    if (v.codec != AVB_VIDEO_CODEC_H264 && v.codec != AVB_VIDEO_CODEC_HEVC &&
        v.codec != AVB_VIDEO_CODEC_VP8 && v.codec != AVB_VIDEO_CODEC_VP9 &&
        v.codec != AVB_VIDEO_CODEC_AV1) {
        enc->set_error("Elementary-stream video encoding supports H.264, HEVC, VP8, VP9 and AV1.");
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    if (v.width <= 0 || v.height <= 0) {
        enc->set_error("Video width/height must be positive.");
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    if (!(v.frame_rate > 0.0) || !std::isfinite(v.frame_rate)) v.frame_rate = 30.0;
    if (v.bitrate <= 0) v.bitrate = k_default_video_bitrate;
    if (v.input_memory == AVB_VIDEO_MEMORY_CPU) {
        if (v.input_format == AVB_PIXEL_FORMAT_UNKNOWN) v.input_format = AVB_PIXEL_FORMAT_BGRA8;
        if (v.input_format != AVB_PIXEL_FORMAT_BGRA8 && v.input_format != AVB_PIXEL_FORMAT_RGBA8 &&
            v.input_format != AVB_PIXEL_FORMAT_NV12 && v.input_format != AVB_PIXEL_FORMAT_I420) {
            enc->set_error("Elementary-stream video input must be BGRA8, RGBA8, NV12 or I420.");
            return AVB_ERROR_INVALID_ARGUMENT;
        }
    }
    if (!valid_profile(v.codec, p.profile)) {
        enc->set_error("The requested profile does not belong to the selected codec.");
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    if (p.gop_size < 0) p.gop_size = 0;
    if (p.low_latency) p.max_b_frames = 0;
    if (p.max_b_frames < -1) p.max_b_frames = -1;
    if (p.max_bitrate < 0) p.max_bitrate = 0;
    if (p.bitstream == AVB_BITSTREAM_AUTO) p.bitstream = AVB_BITSTREAM_ANNEXB;
    if (p.bitstream != AVB_BITSTREAM_ANNEXB && p.bitstream != AVB_BITSTREAM_LENGTH_PREFIXED) {
        enc->set_error("Invalid bitstream format.");
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    enc->video.params = p;
    enc->params.set_codec(v.codec);

    result = enc->impl->open_video(enc->video);
    if (result != AVB_OK) {
        enc->capture_error();
        if (enc->last_error.empty()) enc->set_error("Opening the video stream encoder failed.");
        return result;
    }
    enc->opened = true;
    enc->refresh_extradata();
    return AVB_OK;
}

avb_result avb_stream_encoder_open_audio(avb_stream_encoder **out,
                                         const avb_audio_stream_encode_params *params) {
    if (!out || !params) return AVB_ERROR_INVALID_ARGUMENT;
    avb_result result;
    avb_stream_encoder *enc = new_encoder(params->backend, result);
    *out = enc;
    if (result != AVB_OK) return result;

    avb_audio_stream_encode_params p = *params;
    avb_audio_encode_params &a = p.audio;
    a.enable = 1;
    if (a.codec == AVB_AUDIO_CODEC_AUTO) a.codec = AVB_AUDIO_CODEC_AAC;
    if (a.codec != AVB_AUDIO_CODEC_AAC && a.codec != AVB_AUDIO_CODEC_OPUS) {
        enc->set_error("Elementary-stream audio encoding supports AAC and Opus.");
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    if (a.sample_rate <= 0 || a.channels <= 0 || a.channels > 8) {
        enc->set_error("Audio sample_rate must be positive and channels 1-8.");
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    if (a.codec == AVB_AUDIO_CODEC_OPUS && a.sample_rate != 48000 && a.sample_rate != 24000 &&
        a.sample_rate != 16000 && a.sample_rate != 12000 && a.sample_rate != 8000) {
        enc->set_error("Opus requires sample_rate of 8000/12000/16000/24000/48000.");
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    if (a.bitrate <= 0) a.bitrate = k_default_audio_bitrate;
    enc->audio.params = p;

    result = enc->impl->open_audio(enc->audio);
    if (result != AVB_OK) {
        enc->capture_error();
        if (enc->last_error.empty()) enc->set_error("Opening the audio stream encoder failed.");
        return result;
    }
    enc->opened = true;
    enc->refresh_extradata();
    return AVB_OK;
}

avb_result avb_stream_encoder_get_info(avb_stream_encoder *enc, avb_stream_info *out) {
    if (!enc || !out) return AVB_ERROR_INVALID_ARGUMENT;
    if (!enc->opened) return AVB_ERROR_INVALID_ARGUMENT;
    enc->refresh_extradata();
    *out = {};
    out->is_video = enc->is_video ? 1 : 0;
    out->backend = enc->backend;
    out->backend_name = avb_backend_name(enc->backend);
    const AvbStreamEncoderOutput &o = enc->impl->output();
    out->hardware_device = o.hardware_device;
    out->extradata = enc->extradata.empty() ? nullptr : enc->extradata.data();
    out->extradata_size = (int)enc->extradata.size();
    if (enc->is_video) {
        const avb_video_stream_encode_params &p = enc->video.params;
        out->video_codec = p.video.codec;
        out->codec_name = avb_video_codec_name(p.video.codec);
        out->width = p.video.width;
        out->height = p.video.height;
        out->frame_rate = p.video.frame_rate;
        out->bitstream = enc->h26x() ? p.bitstream : AVB_BITSTREAM_AUTO;
        out->time_base_num = 1;
        out->time_base_den = AVB_STREAM_VIDEO_TIMESCALE;
    } else {
        const avb_audio_encode_params &a = enc->audio.params.audio;
        out->audio_codec = a.codec;
        out->codec_name = avb_audio_codec_name(a.codec);
        out->sample_rate = a.sample_rate;
        out->channels = a.channels;
        out->encoder_delay = o.encoder_delay;
        out->time_base_num = 1;
        out->time_base_den = a.sample_rate;
    }
    return AVB_OK;
}

avb_result avb_stream_encoder_send_video(avb_stream_encoder *enc, const avb_video_frame *frame,
                                         double pts_sec) {
    if (!enc || !frame) return AVB_ERROR_INVALID_ARGUMENT;
    if (!enc->opened || !enc->is_video || enc->flushing) {
        if (enc->opened) enc->set_error("The stream encoder takes no more video.");
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    avb_video_frame input = *frame;
    const avb_video_encode_params &v = enc->video.params.video;
    const char *error = avb::detail::prepare_video_frame(v, input);
    if (!error && (!std::isfinite(pts_sec) || !std::isfinite(frame->pts_sec)))
        error = "Video timestamps must be finite.";
    double pts = pts_sec >= 0.0 ? pts_sec
               : frame->pts_sec >= 0.0 ? frame->pts_sec
               : (double)enc->video_index / v.frame_rate;
    if (!error && pts <= enc->last_video_pts) error = "Video timestamps must increase.";
    if (error) {
        enc->set_error(error);
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    enc->last_error.clear();
    const avb_result res = enc->impl->send_video(input, pts, enc->keyframe_requested);
    if (res != AVB_OK) {
        enc->capture_error();
        return res;
    }
    enc->keyframe_requested = false;
    enc->last_video_pts = pts;
    ++enc->video_index;
    return AVB_OK;
}

avb_result avb_stream_encoder_send_audio_f32(avb_stream_encoder *enc, const float *src,
                                             int frames, double pts_sec) {
    if (!enc || !src || frames <= 0) return AVB_ERROR_INVALID_ARGUMENT;
    if (!enc->opened || enc->is_video || enc->flushing) {
        if (enc->opened) enc->set_error("The stream encoder takes no more audio.");
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    if (!enc->audio_started) {
        if (!std::isfinite(pts_sec)) {
            enc->set_error("Audio timestamps must be finite.");
            return AVB_ERROR_INVALID_ARGUMENT;
        }
        enc->audio_start = pts_sec > 0.0
            ? (int64_t)std::llround(pts_sec * enc->audio.params.audio.sample_rate) : 0;
        enc->audio_started = true;
    }
    enc->last_error.clear();
    const avb_result res = enc->impl->send_audio(src, frames);
    if (res != AVB_OK) enc->capture_error();
    return res;
}

avb_result avb_stream_encoder_receive_packet(avb_stream_encoder *enc, avb_encoded_packet *out) {
    if (!enc || !out) return AVB_ERROR_INVALID_ARGUMENT;
    if (!enc->opened) return AVB_ERROR_INVALID_ARGUMENT;
    if (enc->eof) return AVB_ERROR_EOF;

    AvbStreamEncodedPacket &pkt = enc->packet;
    pkt = AvbStreamEncodedPacket{};
    avb_result res = enc->impl->receive(pkt);
    if (res == AVB_ERROR_EOF) {
        if (!enc->flushing) {
            enc->set_error("The encoder ended its stream unexpectedly.");
            return AVB_ERROR_ENCODE_FAILED;
        }
        enc->eof = true;
        return AVB_ERROR_EOF;
    }
    if (res != AVB_OK) {
        if (res != AVB_ERROR_AGAIN) enc->capture_error();
        return res;
    }
    if (pkt.data.empty()) {
        enc->set_error("The encoder returned an empty packet.");
        return AVB_ERROR_ENCODE_FAILED;
    }

    bool keyframe = pkt.keyframe;
    const uint8_t *data = pkt.data.data();
    size_t size = pkt.data.size();
    int timescale = AVB_STREAM_VIDEO_TIMESCALE;
    int64_t pts = pkt.pts, dts = pkt.dts, duration = pkt.duration;

    if (enc->is_video) {
        const avb_video_stream_encode_params &p = enc->video.params;
        if (enc->h26x()) {
            const avb_video_codec codec = p.video.codec;
            const AvbStreamEncoderOutput &o = enc->impl->output();
            const bool ok = o.length_prefixed
                ? avb::detail::split_length_prefixed(data, size, o.nal_length_size, enc->nals)
                : avb::detail::split_annexb(data, size, enc->nals);
            if (!ok || enc->nals.empty()) {
                enc->set_error("The encoder returned a malformed H.264/HEVC packet.");
                return AVB_ERROR_ENCODE_FAILED;
            }
            if (!enc->params.complete()) {
                const auto &seed = o.extradata;
                if (!seed.empty()) enc->params.load_extradata(seed.data(), seed.size());
            }
            const bool changed = enc->params.update(enc->nals);
            for (const NalRef &nal : enc->nals) {
                if (avb::detail::is_random_access_nal(codec, avb::detail::nal_type(codec, nal.data, nal.size)))
                    keyframe = true;
            }
            const bool lp = p.bitstream == AVB_BITSTREAM_LENGTH_PREFIXED;
            auto append = [&](const uint8_t *nal, size_t n) {
                if (lp) avb::detail::append_length_prefixed(enc->out_data, nal, n);
                else avb::detail::append_annexb(enc->out_data, nal, n);
            };
            enc->out_data.clear();
            if (p.repeat_parameter_sets && keyframe &&
                !packet_has_kind(codec, enc->nals, ParamSetKind::sps) && enc->params.complete()) {
                // Parameter sets go after an access unit delimiter, before the slices.
                size_t first = 0;
                const int aud = codec == AVB_VIDEO_CODEC_HEVC ? 35 : 9;
                if (avb::detail::nal_type(codec, enc->nals[0].data, enc->nals[0].size) == aud) {
                    append(enc->nals[0].data, enc->nals[0].size);
                    first = 1;
                }
                for (const auto *nal : enc->params.all()) append(nal->data(), nal->size());
                for (size_t i = first; i < enc->nals.size(); ++i)
                    append(enc->nals[i].data, enc->nals[i].size);
            } else {
                for (const NalRef &nal : enc->nals) append(nal.data, nal.size);
            }
            if (changed || enc->extradata.empty()) enc->refresh_extradata();
            data = enc->out_data.data();
            size = enc->out_data.size();
        }
        if (duration < 0)
            duration = (int64_t)std::llround(AVB_STREAM_VIDEO_TIMESCALE / p.video.frame_rate);
    } else {
        const avb_audio_encode_params &a = enc->audio.params.audio;
        timescale = a.sample_rate;
        keyframe = true;
        if (a.codec == AVB_AUDIO_CODEC_AAC) {
            const size_t header = avb::detail::aac_adts_header_size(data, size);
            if (header > 0 && header < size) {
                data += header;
                size -= header;
            }
            if (duration < 0) duration = 1024;
        }
        // Audio timestamps can be negative: priming samples come before 0.
        if (pts != -1) pts += enc->audio_start;
        if (dts != -1) dts += enc->audio_start;
    }
    if (dts == -1) dts = pts;

    *out = {};
    out->data = data;
    out->size = (int)size;
    out->pts = pts;
    out->dts = dts;
    out->duration = duration;
    out->time_base_num = 1;
    out->time_base_den = timescale;
    out->pts_sec = pts != -1 ? (double)pts / timescale : -1.0;
    out->duration_sec = duration > 0 ? (double)duration / timescale : 0.0;
    out->keyframe = keyframe ? 1 : 0;
    out->stream_index = 0;
    return AVB_OK;
}

avb_result avb_stream_encoder_request_keyframe(avb_stream_encoder *enc) {
    if (!enc || !enc->opened || !enc->is_video) return AVB_ERROR_INVALID_ARGUMENT;
    enc->keyframe_requested = true;
    return AVB_OK;
}

avb_result avb_stream_encoder_set_bitrate(avb_stream_encoder *enc, int bitrate) {
    if (!enc || !enc->opened || bitrate <= 0) return AVB_ERROR_INVALID_ARGUMENT;
    if (enc->flushing) return AVB_ERROR_INVALID_ARGUMENT;
    enc->last_error.clear();
    const avb_result res = enc->impl->set_bitrate(bitrate);
    if (res != AVB_OK) {
        enc->capture_error();
        return res;
    }
    if (enc->is_video) enc->video.params.video.bitrate = bitrate;
    else enc->audio.params.audio.bitrate = bitrate;
    return AVB_OK;
}

avb_result avb_stream_encoder_flush(avb_stream_encoder *enc) {
    if (!enc || !enc->opened || enc->flushing) return AVB_ERROR_INVALID_ARGUMENT;
    enc->flushing = true;
    enc->last_error.clear();
    const avb_result res = enc->impl->flush();
    if (res != AVB_OK) enc->capture_error();
    return res;
}

const char *avb_stream_encoder_get_last_error(avb_stream_encoder *enc) {
    if (!enc) return nullptr;
    if (!enc->last_error.empty()) return enc->last_error.c_str();
    if (enc->impl) {
        const char *err = enc->impl->get_last_error();
        if (err && err[0]) return err;
    }
    return nullptr;
}

void avb_stream_encoder_close(avb_stream_encoder *enc) {
    delete enc;
}

/* ---------------------------------------------------------------- decoder */

avb_result avb_stream_decoder_open(avb_stream_decoder **out, const avb_stream_decode_params *params) {
    if (!out || !params) return AVB_ERROR_INVALID_ARGUMENT;
    auto *dec = new avb_stream_decoder();
    *out = dec;
    dec->backend = avb::detail::resolve_backend(params->backend);
    dec->impl = avb_create_stream_decoder_backend(params->backend);
    if (!dec->impl) {
        dec->set_error("Requested backend is not available for elementary-stream decoding on this platform.");
        return AVB_ERROR_BACKEND_NOT_AVAILABLE;
    }

    avb_stream_decode_params p = *params;
    const unsigned char *extradata = p.extradata;
    const size_t extradata_size = p.extradata_size > 0 ? (size_t)p.extradata_size : 0;
    p.extradata = nullptr;
    p.extradata_size = 0;
    if (extradata_size > 0 && !extradata) {
        dec->set_error("extradata is NULL but extradata_size is positive.");
        return AVB_ERROR_INVALID_ARGUMENT;
    }

    AvbStreamDecodeConfig config;
    if (p.is_video) {
        if (p.video_codec == AVB_VIDEO_CODEC_AUTO) p.video_codec = AVB_VIDEO_CODEC_H264;
        if (p.video_codec != AVB_VIDEO_CODEC_H264 && p.video_codec != AVB_VIDEO_CODEC_HEVC &&
            p.video_codec != AVB_VIDEO_CODEC_VP8 && p.video_codec != AVB_VIDEO_CODEC_VP9 &&
            p.video_codec != AVB_VIDEO_CODEC_AV1) {
            dec->set_error("Elementary-stream video decoding supports H.264, HEVC, VP8, VP9 and AV1.");
            return AVB_ERROR_INVALID_ARGUMENT;
        }
        if (p.video_memory == AVB_VIDEO_MEMORY_CPU) {
            if (p.video_format == AVB_PIXEL_FORMAT_UNKNOWN) p.video_format = AVB_PIXEL_FORMAT_BGRA8;
            if (p.video_format != AVB_PIXEL_FORMAT_BGRA8 && p.video_format != AVB_PIXEL_FORMAT_RGBA8 &&
                p.video_format != AVB_PIXEL_FORMAT_NV12 && p.video_format != AVB_PIXEL_FORMAT_I420) {
                dec->set_error("Elementary-stream video output must be BGRA8, RGBA8, NV12 or I420.");
                return AVB_ERROR_INVALID_ARGUMENT;
            }
        }
        if (p.bitstream != AVB_BITSTREAM_AUTO && p.bitstream != AVB_BITSTREAM_ANNEXB &&
            p.bitstream != AVB_BITSTREAM_LENGTH_PREFIXED) {
            dec->set_error("Invalid bitstream format.");
            return AVB_ERROR_INVALID_ARGUMENT;
        }
        dec->h26x = avb::detail::is_h26x(p.video_codec);
        if (dec->h26x) {
            dec->ps.set_codec(p.video_codec);
            const int length_size = dec->ps.load_extradata(extradata, extradata_size);
            if (length_size > 0) dec->nal_length_size = length_size;
            dec->config = dec->ps.configuration_record();
            config.extradata = dec->config;
        } else {
            dec->waiting_keyframe = false;
            if (extradata_size > 0) config.extradata.assign(extradata, extradata + extradata_size);
        }
    } else {
        dec->waiting_keyframe = false;
        if (p.audio_codec == AVB_AUDIO_CODEC_AUTO) p.audio_codec = AVB_AUDIO_CODEC_AAC;
        if (p.audio_codec == AVB_AUDIO_CODEC_AAC) {
            if (extradata_size > 0) {
                dec->audio_extra.assign(extradata, extradata + extradata_size);
                // An ES descriptor (esds content) carries the ASC inside it.
                if (dec->audio_extra[0] == 0x03 || (extradata_size > 4 && extradata[4] == 0x03)) {
                    auto asc = avb::detail::aac_asc_from_esds(extradata, extradata_size);
                    if (!asc.empty()) dec->audio_extra = std::move(asc);
                }
            } else if (p.sample_rate > 0 && p.channels > 0) {
                dec->audio_extra = avb::detail::aac_audio_specific_config(p.sample_rate, p.channels);
            } else {
                dec->set_error("AAC decoding needs an AudioSpecificConfig, or sample_rate and channels.");
                return AVB_ERROR_INVALID_ARGUMENT;
            }
            int rate = 0, channels = 0, object = 0;
            if (!avb::detail::aac_parse_audio_specific_config(
                    dec->audio_extra.data(), dec->audio_extra.size(), rate, channels, object)) {
                dec->set_error("Invalid AAC AudioSpecificConfig.");
                return AVB_ERROR_INVALID_ARGUMENT;
            }
            if (p.sample_rate <= 0) p.sample_rate = rate;
            if (p.channels <= 0) p.channels = channels;
        } else if (p.audio_codec == AVB_AUDIO_CODEC_OPUS) {
            int channels = 0, pre_skip = 0, rate = 0;
            if (extradata_size > 0 &&
                avb::detail::opus_parse_head(extradata, extradata_size, channels, pre_skip, rate)) {
                dec->audio_extra.assign(extradata, extradata + extradata_size);
                if (p.channels <= 0) p.channels = channels;
            } else {
                if (p.channels <= 0) p.channels = 2;
                dec->audio_extra = avb::detail::opus_head(p.channels, 0, 48000);
            }
            // Opus always decodes at 48 kHz unless asked otherwise.
            if (p.sample_rate <= 0) p.sample_rate = 48000;
        } else {
            dec->set_error("Elementary-stream audio decoding supports AAC and Opus.");
            return AVB_ERROR_INVALID_ARGUMENT;
        }
        config.extradata = dec->audio_extra;
    }
    dec->params = p;
    config.params = p;

    const avb_result res = dec->impl->open(config);
    if (res != AVB_OK) {
        dec->capture_error();
        if (dec->last_error.empty()) dec->set_error("Opening the stream decoder failed.");
        return res;
    }
    dec->opened = true;
    return AVB_OK;
}

avb_result avb_stream_decoder_send_packet(avb_stream_decoder *dec, const avb_encoded_packet *packet) {
    if (!dec || !dec->opened) return AVB_ERROR_INVALID_ARGUMENT;
    if (!packet) {
        if (dec->draining) return AVB_OK;
        dec->draining = true;
        AvbStreamDecodeInput drain;
        const avb_result res = dec->impl->send(drain);
        if (res != AVB_OK) dec->capture_error();
        return res;
    }
    if (dec->draining) {
        dec->set_error("The stream decoder is draining; reset it before sending more packets.");
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    if (!packet->data || packet->size <= 0) {
        dec->set_error("Packet has no data.");
        return AVB_ERROR_INVALID_ARGUMENT;
    }

    AvbStreamDecodeInput input;
    input.pts_sec = packet->pts >= 0 && packet->time_base_num > 0 && packet->time_base_den > 0
        ? (double)packet->pts * packet->time_base_num / packet->time_base_den
        : packet->pts_sec;
    if (!std::isfinite(input.pts_sec)) input.pts_sec = -1.0;
    input.keyframe = packet->keyframe != 0;
    const uint8_t *data = packet->data;
    size_t size = (size_t)packet->size;

    if (dec->h26x) {
        const avb_video_codec codec = dec->params.video_codec;
        const bool annexb = dec->params.bitstream == AVB_BITSTREAM_ANNEXB ||
            (dec->params.bitstream == AVB_BITSTREAM_AUTO &&
             avb::detail::starts_with_start_code(data, size));
        const bool ok = annexb
            ? avb::detail::split_annexb(data, size, dec->nals)
            : avb::detail::split_length_prefixed(data, size, dec->nal_length_size, dec->nals);
        if (!ok) {
            dec->set_error("Malformed H.264/HEVC packet framing.");
            return AVB_ERROR_DECODE_FAILED;
        }
        if (dec->ps.update(dec->nals)) {
            dec->config = dec->ps.configuration_record();
            dec->config_pending = true;
        }
        bool keyframe = false, has_vcl = false;
        for (const NalRef &nal : dec->nals) {
            const int type = avb::detail::nal_type(codec, nal.data, nal.size);
            keyframe = keyframe || avb::detail::is_random_access_nal(codec, type);
            has_vcl = has_vcl || avb::detail::is_vcl_nal(codec, type);
        }
        if (dec->waiting_keyframe) {
            if (!keyframe || !dec->ps.complete()) return AVB_OK; // dropped
            dec->waiting_keyframe = false;
        }
        if (!has_vcl) return AVB_OK; // parameter sets / SEI only

        dec->input.clear();
        if (dec->impl->wants_length_prefixed()) {
            for (const NalRef &nal : dec->nals) {
                const int type = avb::detail::nal_type(codec, nal.data, nal.size);
                if (avb::detail::param_set_kind(codec, type) != ParamSetKind::none) continue;
                avb::detail::append_length_prefixed(dec->input, nal.data, nal.size);
            }
        } else {
            if (keyframe && !packet_has_kind(codec, dec->nals, ParamSetKind::sps)) {
                for (const auto *nal : dec->ps.all())
                    avb::detail::append_annexb(dec->input, nal->data(), nal->size());
            }
            for (const NalRef &nal : dec->nals)
                avb::detail::append_annexb(dec->input, nal.data, nal.size);
        }
        input.data = dec->input.data();
        input.size = dec->input.size();
        input.keyframe = keyframe;
        input.config_changed = dec->config_pending;
        input.config = &dec->config;
    } else {
        if (dec->params.is_video && dec->params.video_codec == AVB_VIDEO_CODEC_VP8)
            input.keyframe = (data[0] & 1) == 0;
        if (!dec->params.is_video && dec->params.audio_codec == AVB_AUDIO_CODEC_AAC) {
            const size_t header = avb::detail::aac_adts_header_size(data, size);
            if (header > 0 && header < size) {
                data += header;
                size -= header;
            }
        }
        input.data = data;
        input.size = size;
    }

    dec->last_error.clear();
    const avb_result res = dec->impl->send(input);
    if (res != AVB_OK) {
        dec->capture_error();
        return res;
    }
    if (input.config_changed) dec->config_pending = false;
    return AVB_OK;
}

avb_result avb_stream_decoder_receive_video_frame(avb_stream_decoder *dec, avb_video_frame *out) {
    if (!dec || !out || !dec->opened || !dec->params.is_video) return AVB_ERROR_INVALID_ARGUMENT;
    const avb_result res = dec->impl->receive_video(*out);
    if (res == AVB_OK) {
        dec->width = out->width;
        dec->height = out->height;
    } else if (res != AVB_ERROR_AGAIN && res != AVB_ERROR_EOF) {
        dec->capture_error();
    }
    return res;
}

void avb_stream_decoder_release_video_frame(avb_stream_decoder *dec, avb_video_frame *frame) {
    if (!dec || !frame || !dec->impl) return;
    dec->impl->release_video(*frame);
}

avb_result avb_stream_decoder_receive_audio_f32(avb_stream_decoder *dec, float *dst, int max_frames,
                                                int *out_frames, double *out_pts_sec) {
    if (out_frames) *out_frames = 0;
    if (out_pts_sec) *out_pts_sec = -1.0;
    if (!dec || !dst || max_frames <= 0 || !dec->opened || dec->params.is_video)
        return AVB_ERROR_INVALID_ARGUMENT;
    if (dec->audio_pos >= dec->audio_frames) {
        int frames = 0;
        double pts = -1.0;
        const avb_result res = dec->impl->receive_audio(dec->audio, frames, pts);
        if (res != AVB_OK) {
            if (res != AVB_ERROR_AGAIN && res != AVB_ERROR_EOF) dec->capture_error();
            return res;
        }
        dec->audio_frames = frames;
        dec->audio_pos = 0;
        dec->audio_pts = pts;
        if (frames <= 0) return AVB_ERROR_AGAIN;
    }
    int rate = 0, channels = 0;
    dec->impl->audio_format(rate, channels);
    if (channels <= 0) {
        dec->set_error("Decoded audio has no channel count.");
        return AVB_ERROR_DECODE_FAILED;
    }
    const int n = std::min(max_frames, dec->audio_frames - dec->audio_pos);
    std::memcpy(dst, dec->audio.data() + (size_t)dec->audio_pos * channels,
                (size_t)n * channels * sizeof(float));
    if (out_pts_sec && dec->audio_pts >= 0.0 && rate > 0)
        *out_pts_sec = dec->audio_pts + (double)dec->audio_pos / rate;
    dec->audio_pos += n;
    if (out_frames) *out_frames = n;
    return AVB_OK;
}

avb_result avb_stream_decoder_get_info(avb_stream_decoder *dec, avb_stream_info *out) {
    if (!dec || !out || !dec->opened) return AVB_ERROR_INVALID_ARGUMENT;
    *out = {};
    const avb_stream_decode_params &p = dec->params;
    out->is_video = p.is_video ? 1 : 0;
    out->backend = dec->backend;
    out->backend_name = avb_backend_name(dec->backend);
    out->hardware_device = dec->impl->hardware_device();
    if (p.is_video) {
        out->video_codec = p.video_codec;
        out->codec_name = avb_video_codec_name(p.video_codec);
        out->width = dec->width;
        out->height = dec->height;
        out->time_base_num = 1;
        out->time_base_den = AVB_STREAM_VIDEO_TIMESCALE;
        if (dec->h26x) {
            out->bitstream = p.bitstream;
            out->extradata = dec->config.empty() ? nullptr : dec->config.data();
            out->extradata_size = (int)dec->config.size();
        }
    } else {
        out->audio_codec = p.audio_codec;
        out->codec_name = avb_audio_codec_name(p.audio_codec);
        dec->impl->audio_format(out->sample_rate, out->channels);
        out->time_base_num = 1;
        out->time_base_den = out->sample_rate > 0 ? out->sample_rate : p.sample_rate;
        out->extradata = dec->audio_extra.empty() ? nullptr : dec->audio_extra.data();
        out->extradata_size = (int)dec->audio_extra.size();
    }
    return AVB_OK;
}

avb_result avb_stream_decoder_reset(avb_stream_decoder *dec) {
    if (!dec || !dec->opened) return AVB_ERROR_INVALID_ARGUMENT;
    dec->last_error.clear();
    const avb_result res = dec->impl->reset();
    if (res != AVB_OK) {
        dec->capture_error();
        return res;
    }
    dec->draining = false;
    dec->waiting_keyframe = dec->h26x;
    // The backend dropped its configuration with its state; hand the known
    // parameter sets over again with the next keyframe.
    dec->config_pending = dec->h26x && !dec->config.empty();
    dec->audio_frames = dec->audio_pos = 0;
    dec->audio_pts = -1.0;
    return AVB_OK;
}

const char *avb_stream_decoder_get_last_error(avb_stream_decoder *dec) {
    if (!dec) return nullptr;
    if (!dec->last_error.empty()) return dec->last_error.c_str();
    if (dec->impl) {
        const char *err = dec->impl->get_last_error();
        if (err && err[0]) return err;
    }
    return nullptr;
}

void avb_stream_decoder_close(avb_stream_decoder *dec) {
    delete dec;
}

} // extern "C"
