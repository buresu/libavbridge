#include "backends/ffmpeg/avb_runtime_ffmpeg.hpp"
#include "avb_capability_builder.hpp"

#if defined(AVB_ENABLE_FFMPEG)

#include "backends/ffmpeg/avb_ffmpeg_loader.hpp"

using avb::detail::Container;
using avb::detail::audio_only_container;
using avb::capability::add_audio_codec;
using avb::capability::add_device;
using avb::capability::add_memory;
using avb::capability::add_software_pixel_formats;
using avb::capability::add_video_codec;

namespace {

AVCodecID video_codec_id(avb_video_codec codec) {
    switch (codec) {
        case AVB_VIDEO_CODEC_H264: return AV_CODEC_ID_H264;
        case AVB_VIDEO_CODEC_HEVC: return AV_CODEC_ID_HEVC;
        case AVB_VIDEO_CODEC_VP8:  return AV_CODEC_ID_VP8;
        case AVB_VIDEO_CODEC_VP9:  return AV_CODEC_ID_VP9;
        case AVB_VIDEO_CODEC_AV1:  return AV_CODEC_ID_AV1;
        default: return AV_CODEC_ID_NONE;
    }
}

AVCodecID audio_codec_id(avb_audio_codec codec) {
    switch (codec) {
        case AVB_AUDIO_CODEC_AAC:     return AV_CODEC_ID_AAC;
        case AVB_AUDIO_CODEC_OPUS:    return AV_CODEC_ID_OPUS;
        case AVB_AUDIO_CODEC_MP3:     return AV_CODEC_ID_MP3;
        case AVB_AUDIO_CODEC_FLAC:    return AV_CODEC_ID_FLAC;
        case AVB_AUDIO_CODEC_VORBIS:  return AV_CODEC_ID_VORBIS;
        case AVB_AUDIO_CODEC_PCM_S16: return AV_CODEC_ID_PCM_S16LE;
        case AVB_AUDIO_CODEC_PCM_F32: return AV_CODEC_ID_PCM_F32LE;
        default: return AV_CODEC_ID_NONE;
    }
}

AVHWDeviceType hardware_device_type(avb_hardware_device device) {
    switch (device) {
        case AVB_HW_DEVICE_VAAPI:        return AV_HWDEVICE_TYPE_VAAPI;
        case AVB_HW_DEVICE_CUDA:         return AV_HWDEVICE_TYPE_CUDA;
        case AVB_HW_DEVICE_QSV:          return AV_HWDEVICE_TYPE_QSV;
        case AVB_HW_DEVICE_D3D11VA:      return AV_HWDEVICE_TYPE_D3D11VA;
        case AVB_HW_DEVICE_VIDEOTOOLBOX: return AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
        case AVB_HW_DEVICE_AMF:          return AV_HWDEVICE_TYPE_D3D11VA;
        default:                         return AV_HWDEVICE_TYPE_NONE;
    }
}

bool device_available(
    const AvbFFmpegFuncs &ff,
    avb_hardware_device device) {
    AVHWDeviceType type = hardware_device_type(device);
    if (type == AV_HWDEVICE_TYPE_NONE) return false;

    AVBufferRef *context = nullptr;
    int result = ff.av_hwdevice_ctx_create(
        &context, type, nullptr, nullptr, 0);
    if (context) ff.av_buffer_unref(&context);
    return result >= 0;
}

const char *const *hardware_encoder_names(
    avb_video_codec codec,
    avb_hardware_device device) {
    static const char *h264_vaapi[] = {"h264_vaapi", nullptr};
    static const char *hevc_vaapi[] = {"hevc_vaapi", nullptr};
    static const char *vp8_vaapi[] = {"vp8_vaapi", nullptr};
    static const char *vp9_vaapi[] = {"vp9_vaapi", nullptr};
    static const char *av1_vaapi[] = {"av1_vaapi", nullptr};
    static const char *none[] = {nullptr};

    if (device != AVB_HW_DEVICE_VAAPI) return none;
    switch (codec) {
        case AVB_VIDEO_CODEC_H264: return h264_vaapi;
        case AVB_VIDEO_CODEC_HEVC: return hevc_vaapi;
        case AVB_VIDEO_CODEC_VP8:  return vp8_vaapi;
        case AVB_VIDEO_CODEC_VP9:  return vp9_vaapi;
        case AVB_VIDEO_CODEC_AV1:  return av1_vaapi;
        default: return none;
    }
}

bool has_hardware_encoder(
    const AvbFFmpegFuncs &ff,
    avb_video_codec codec,
    avb_hardware_device device) {
    if (!device_available(ff, device)) return false;

    const char *const *names = hardware_encoder_names(codec, device);
    for (int i = 0; names[i]; ++i) {
        if (ff.avcodec_find_encoder_by_name(names[i])) return true;
    }
    return false;
}

bool has_hardware_decoder(
    const AvbFFmpegFuncs &ff,
    avb_video_codec codec,
    avb_hardware_device device) {
    const AVCodec *decoder = ff.avcodec_find_decoder(video_codec_id(codec));
    if (!decoder || !device_available(ff, device)) return false;

    AVHWDeviceType type = hardware_device_type(device);
    for (int i = 0;; ++i) {
        const AVCodecHWConfig *config = ff.avcodec_get_hw_config(decoder, i);
        if (!config) break;
        if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
            config->device_type == type) {
            return true;
        }
    }
    return false;
}

bool has_video_encoder(const AvbFFmpegFuncs &ff, avb_video_codec codec) {
    AVCodecID id = video_codec_id(codec);
    return (id != AV_CODEC_ID_NONE && ff.avcodec_find_encoder(id)) ||
           has_hardware_encoder(ff, codec, AVB_HW_DEVICE_VAAPI);
}

bool has_audio_encoder(const AvbFFmpegFuncs &ff, avb_audio_codec codec) {
    const char *preferred = nullptr;
    switch (codec) {
        case AVB_AUDIO_CODEC_OPUS:   preferred = "libopus"; break;
        case AVB_AUDIO_CODEC_MP3:    preferred = "libmp3lame"; break;
        case AVB_AUDIO_CODEC_VORBIS: preferred = "libvorbis"; break;
        default: break;
    }
    if (preferred && ff.avcodec_find_encoder_by_name(preferred)) return true;

    AVCodecID id = audio_codec_id(codec);
    return id != AV_CODEC_ID_NONE && ff.avcodec_find_encoder(id);
}

void fill_decoder(
    avb_decoder_capabilities &out,
    Container container,
    const AvbFFmpegFuncs &ff) {
    const avb_video_codec video_codecs[] = {
        AVB_VIDEO_CODEC_H264, AVB_VIDEO_CODEC_HEVC, AVB_VIDEO_CODEC_VP8,
        AVB_VIDEO_CODEC_VP9, AVB_VIDEO_CODEC_AV1
    };
    const avb_audio_codec audio_codecs[] = {
        AVB_AUDIO_CODEC_AAC, AVB_AUDIO_CODEC_OPUS, AVB_AUDIO_CODEC_MP3,
        AVB_AUDIO_CODEC_FLAC, AVB_AUDIO_CODEC_VORBIS,
        AVB_AUDIO_CODEC_PCM_S16, AVB_AUDIO_CODEC_PCM_F32
    };

    if (!audio_only_container(container)) {
        for (avb_video_codec codec : video_codecs) {
            if (ff.avcodec_find_decoder(video_codec_id(codec)))
                add_video_codec(out, codec, container);
        }
    }
    for (avb_audio_codec codec : audio_codecs) {
        if (ff.avcodec_find_decoder(audio_codec_id(codec)))
            add_audio_codec(out, codec, container);
    }

    add_software_pixel_formats(out);
    add_memory(out, AVB_VIDEO_MEMORY_CPU);
    add_device(out, AVB_HW_DEVICE_AUTO);

    bool has_hardware = false;
    if (device_available(ff, AVB_HW_DEVICE_VAAPI)) {
        add_device(out, AVB_HW_DEVICE_VAAPI);
        has_hardware = true;
    }
    for (avb_video_codec codec : video_codecs) {
        if (!has_hardware &&
            has_hardware_decoder(ff, codec, AVB_HW_DEVICE_VAAPI)) {
            has_hardware = true;
        }
    }
    if (has_hardware) {
        add_memory(out, AVB_VIDEO_MEMORY_NATIVE);
#if defined(__linux__)
        add_memory(out, AVB_VIDEO_MEMORY_DMABUF);
#endif
    }
}

void fill_encoder(
    avb_encoder_capabilities &out,
    Container container,
    const AvbFFmpegFuncs &ff) {
    const avb_video_codec video_codecs[] = {
        AVB_VIDEO_CODEC_H264, AVB_VIDEO_CODEC_HEVC, AVB_VIDEO_CODEC_VP8,
        AVB_VIDEO_CODEC_VP9, AVB_VIDEO_CODEC_AV1
    };
    const avb_audio_codec audio_codecs[] = {
        AVB_AUDIO_CODEC_AAC, AVB_AUDIO_CODEC_OPUS, AVB_AUDIO_CODEC_MP3,
        AVB_AUDIO_CODEC_FLAC, AVB_AUDIO_CODEC_VORBIS,
        AVB_AUDIO_CODEC_PCM_S16, AVB_AUDIO_CODEC_PCM_F32
    };

    if (!audio_only_container(container)) {
        for (avb_video_codec codec : video_codecs) {
            if (has_video_encoder(ff, codec))
                add_video_codec(out, codec, container);
        }
    }
    for (avb_audio_codec codec : audio_codecs) {
        if (has_audio_encoder(ff, codec))
            add_audio_codec(out, codec, container);
    }

    add_memory(out, AVB_VIDEO_MEMORY_CPU);
    add_device(out, AVB_HW_DEVICE_AUTO);
    if (device_available(ff, AVB_HW_DEVICE_VAAPI)) {
        add_device(out, AVB_HW_DEVICE_VAAPI);
        add_memory(out, AVB_VIDEO_MEMORY_NATIVE);
#if defined(__linux__)
        add_memory(out, AVB_VIDEO_MEMORY_DMABUF);
#endif
    }
}

}  // namespace

bool avb_probe_ffmpeg_decoder(
    avb_decoder_capabilities &out,
    Container container) {
    AvbFFmpegFuncs ff{};
    char error[AVB_MAX_ERROR] = {};
    if (!avb_ffmpeg_load(ff, error, sizeof(error))) return false;

    fill_decoder(out, container, ff);
    return true;
}

bool avb_probe_ffmpeg_encoder(
    avb_encoder_capabilities &out,
    Container container) {
    AvbFFmpegFuncs ff{};
    char error[AVB_MAX_ERROR] = {};
    if (!avb_ffmpeg_load(ff, error, sizeof(error))) return false;

    fill_encoder(out, container, ff);
    return true;
}

#endif
