#include "avbridge.h"
#include "avb_capability_common.hpp"

#if defined(AVB_ENABLE_FFMPEG)
#include "backends/ffmpeg/avb_ffmpeg_loader.hpp"
#endif

#if defined(AVB_ENABLE_GSTREAMER)
#include "backends/gstreamer/avb_gstreamer_loader.hpp"
#endif

#if defined(AVB_ENABLE_MEDIAFOUNDATION)
#include <mfapi.h>
#include <mfidl.h>
#include <codecapi.h>
#include <wrl/client.h>
#endif

#if defined(AVB_ENABLE_AVFOUNDATION)
#include <VideoToolbox/VideoToolbox.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace {

using avb::detail::Container;
using avb::detail::audio_only_container;
using avb::detail::container_accepts_audio;
using avb::detail::container_accepts_video;
using avb::detail::container_from_path;
using avb::detail::container_name;
using avb::detail::resolve_backend;

#if defined(AVB_ENABLE_MEDIAFOUNDATION)
using Microsoft::WRL::ComPtr;
#endif

static void add_video_codec(avb_decoder_capabilities &out, avb_video_codec codec,
                            Container c) {
    if (!container_accepts_video(c, codec)) return;
    if (out.video_codec_count >= AVB_MAX_CODEC_CAPS) return;
    out.video_codecs[out.video_codec_count++] = codec;
}

static void add_video_codec(avb_encoder_capabilities &out, avb_video_codec codec,
                            Container c) {
    if (!container_accepts_video(c, codec)) return;
    if (out.video_codec_count >= AVB_MAX_CODEC_CAPS) return;
    out.video_codecs[out.video_codec_count++] = codec;
}

static void add_audio_codec(avb_decoder_capabilities &out, avb_audio_codec codec,
                            Container c) {
    if (!container_accepts_audio(c, codec)) return;
    if (out.audio_codec_count >= AVB_MAX_CODEC_CAPS) return;
    out.audio_codecs[out.audio_codec_count++] = codec;
}

static void add_audio_codec(avb_encoder_capabilities &out, avb_audio_codec codec,
                            Container c) {
    if (!container_accepts_audio(c, codec)) return;
    if (out.audio_codec_count >= AVB_MAX_CODEC_CAPS) return;
    out.audio_codecs[out.audio_codec_count++] = codec;
}

static void add_audio_codec_unchecked(avb_encoder_capabilities &out,
                                      avb_audio_codec codec) {
    if (out.audio_codec_count >= AVB_MAX_CODEC_CAPS) return;
    out.audio_codecs[out.audio_codec_count++] = codec;
}

static bool mediafoundation_video_container(Container c) {
    return c == Container::any || c == Container::mp4 ||
           c == Container::mov || c == Container::ivf ||
           c == Container::unknown;
}

static void add_pixel_format(avb_decoder_capabilities &out,
                             avb_pixel_format format) {
    if (out.pixel_format_count >= AVB_MAX_PIXEL_FORMAT_CAPS) return;
    out.pixel_formats[out.pixel_format_count++] = format;
}

static void add_software_pixel_formats(avb_decoder_capabilities &out) {
    add_pixel_format(out, AVB_PIXEL_FORMAT_BGRA8);
    add_pixel_format(out, AVB_PIXEL_FORMAT_RGBA8);
    add_pixel_format(out, AVB_PIXEL_FORMAT_NV12);
    add_pixel_format(out, AVB_PIXEL_FORMAT_I420);
}

static void add_memory(avb_decoder_capabilities &out, avb_video_memory_type memory) {
    if (out.video_memory_count >= AVB_MAX_VIDEO_MEMORY_CAPS) return;
    out.video_memory[out.video_memory_count++] = memory;
}

static void add_memory(avb_encoder_capabilities &out, avb_video_memory_type memory) {
    if (out.video_memory_count >= AVB_MAX_VIDEO_MEMORY_CAPS) return;
    out.video_memory[out.video_memory_count++] = memory;
}

static void add_device(avb_decoder_capabilities &out, avb_hardware_device device) {
    if (out.hardware_device_count >= AVB_MAX_HARDWARE_DEVICE_CAPS) return;
    out.hardware_devices[out.hardware_device_count++] = device;
}

static void add_device(avb_encoder_capabilities &out, avb_hardware_device device) {
    if (out.hardware_device_count >= AVB_MAX_HARDWARE_DEVICE_CAPS) return;
    out.hardware_devices[out.hardware_device_count++] = device;
}

static void fill_platform_decoder(avb_decoder_capabilities &out, Container c) {
    if (!audio_only_container(c)) {
        add_video_codec(out, AVB_VIDEO_CODEC_H264, c);
        add_video_codec(out, AVB_VIDEO_CODEC_HEVC, c);
    }
    add_audio_codec(out, AVB_AUDIO_CODEC_AAC, c);
    add_audio_codec(out, AVB_AUDIO_CODEC_MP3, c);
    add_software_pixel_formats(out);
    add_memory(out, AVB_VIDEO_MEMORY_CPU);
    add_device(out, AVB_HW_DEVICE_AUTO);
}

static void fill_platform_encoder(avb_encoder_capabilities &out, Container c) {
    if (!audio_only_container(c)) {
        add_video_codec(out, AVB_VIDEO_CODEC_H264, c);
        add_video_codec(out, AVB_VIDEO_CODEC_HEVC, c);
    }
    add_audio_codec(out, AVB_AUDIO_CODEC_AAC, c);
    add_memory(out, AVB_VIDEO_MEMORY_CPU);
    add_device(out, AVB_HW_DEVICE_AUTO);
}

#if defined(AVB_ENABLE_MEDIAFOUNDATION)
// Media Foundation runtime probing is intentionally MFT-based. It answers
// "does this Windows runtime advertise a transform for this subtype?" while the
// container filter above keeps obviously incompatible file extensions out.
// SourceReader/SinkWriter can still reject a concrete session later because of
// profile, bitrate, resolution, or media-sink limits.
struct MfStartupScope {
    bool started = false;

    MfStartupScope() {
        started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    }

    ~MfStartupScope() {
        if (started) MFShutdown();
    }
};

static uint32_t mf_fourcc(const char (&s)[5]) {
    return ((uint32_t)(unsigned char)s[0]) |
           ((uint32_t)(unsigned char)s[1] << 8) |
           ((uint32_t)(unsigned char)s[2] << 16) |
           ((uint32_t)(unsigned char)s[3] << 24);
}

static GUID mf_subtype_from_fourcc(const char (&s)[5]) {
    GUID g = { mf_fourcc(s), 0x0000, 0x0010,
        { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
    return g;
}

static GUID mf_subtype_from_wave_tag(uint32_t tag) {
    GUID g = { tag, 0x0000, 0x0010,
        { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
    return g;
}

static GUID mf_vorbis_subtype() {
    GUID g = { 0x8d2fd10b, 0x5841, 0x4a6b,
        { 0x89, 0x05, 0x58, 0x8f, 0xec, 0x1a, 0xde, 0xd9 } };
    return g;
}

static bool mf_has_mft(const GUID &category, const GUID &major,
                       const GUID &subtype, bool encoder) {
    MFT_REGISTER_TYPE_INFO type{};
    type.guidMajorType = major;
    type.guidSubtype = subtype;

    IMFActivate **activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(category, MFT_ENUM_FLAG_ALL,
                           encoder ? nullptr : &type,
                           encoder ? &type : nullptr,
                           &activates, &count);
    if (activates) {
        for (UINT32 i = 0; i < count; ++i) {
            if (activates[i]) activates[i]->Release();
        }
        CoTaskMemFree(activates);
    }
    return SUCCEEDED(hr) && count > 0;
}

static bool mf_has_any_mft(const GUID &category, const GUID &major,
                           const GUID *subtypes, int subtype_count,
                           bool encoder) {
    for (int i = 0; i < subtype_count; ++i) {
        if (mf_has_mft(category, major, subtypes[i], encoder)) return true;
    }
    return false;
}

static int mf_video_subtypes(avb_video_codec codec, GUID *out, int max_count) {
    if (!out || max_count <= 0) return 0;
    switch (codec) {
        case AVB_VIDEO_CODEC_H264:
            out[0] = MFVideoFormat_H264;
            return 1;
        case AVB_VIDEO_CODEC_HEVC:
            if (max_count < 2) return 0;
            out[0] = MFVideoFormat_HEVC;
            out[1] = mf_subtype_from_fourcc("H265");
            return 2;
        case AVB_VIDEO_CODEC_VP8:
            out[0] = mf_subtype_from_fourcc("VP80");
            return 1;
        case AVB_VIDEO_CODEC_VP9:
            out[0] = mf_subtype_from_fourcc("VP90");
            return 1;
        case AVB_VIDEO_CODEC_AV1:
            out[0] = mf_subtype_from_fourcc("AV01");
            return 1;
        default:
            return 0;
    }
}

static bool mf_has_video_decoder(avb_video_codec codec) {
    GUID subtypes[2] = {};
    int count = mf_video_subtypes(codec, subtypes, 2);
    return mf_has_any_mft(MFT_CATEGORY_VIDEO_DECODER, MFMediaType_Video,
                          subtypes, count, false);
}

static bool mf_has_video_encoder(avb_video_codec codec) {
    GUID subtypes[2] = {};
    int count = mf_video_subtypes(codec, subtypes, 2);
    return mf_has_any_mft(MFT_CATEGORY_VIDEO_ENCODER, MFMediaType_Video,
                          subtypes, count, true);
}

static bool mf_can_configure_video_encoder(avb_video_codec codec) {
    GUID subtypes[2] = {};
    int count = mf_video_subtypes(codec, subtypes, 2);
    if (count <= 0) return false;

    MFT_REGISTER_TYPE_INFO type{};
    type.guidMajorType = MFMediaType_Video;
    type.guidSubtype = subtypes[0];

    IMFActivate **activates = nullptr;
    UINT32 activate_count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_ALL,
                           nullptr, &type, &activates, &activate_count);
    if (FAILED(hr) || activate_count == 0) {
        if (activates) CoTaskMemFree(activates);
        return false;
    }

    bool ok = false;
    for (UINT32 i = 0; i < activate_count && !ok; ++i) {
        ComPtr<IMFTransform> encoder;
        if (!activates[i] ||
            FAILED(activates[i]->ActivateObject(IID_PPV_ARGS(&encoder))) ||
            !encoder) {
            continue;
        }

        ComPtr<IMFAttributes> attributes;
        UINT32 is_async = FALSE;
        if (SUCCEEDED(encoder->GetAttributes(&attributes)) && attributes)
            attributes->GetUINT32(MF_TRANSFORM_ASYNC, &is_async);
        if (is_async) {
            if (!attributes ||
                FAILED(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE))) {
                activates[i]->ShutdownObject();
                continue;
            }
        }

        ComPtr<IMFMediaType> out_type;
        MFCreateMediaType(&out_type);
        out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out_type->SetGUID(MF_MT_SUBTYPE, subtypes[0]);
        out_type->SetUINT32(MF_MT_AVG_BITRATE, 1000000);
        out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (codec == AVB_VIDEO_CODEC_AV1)
            out_type->SetUINT32(
                MF_MT_MPEG2_PROFILE, eAVEncAV1VProfile_Main_420_8);
        MFSetAttributeSize(out_type.Get(), MF_MT_FRAME_SIZE, 320, 240);
        MFSetAttributeRatio(out_type.Get(), MF_MT_FRAME_RATE, 30, 1);
        MFSetAttributeRatio(out_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        ComPtr<IMFMediaType> in_type;
        MFCreateMediaType(&in_type);
        in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(in_type.Get(), MF_MT_FRAME_SIZE, 320, 240);
        MFSetAttributeRatio(in_type.Get(), MF_MT_FRAME_RATE, 30, 1);
        MFSetAttributeRatio(in_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        in_type->SetUINT32(MF_MT_DEFAULT_STRIDE, 320);

        HRESULT out_hr = encoder->SetOutputType(0, out_type.Get(), 0);
        HRESULT in_hr = SUCCEEDED(out_hr)
            ? encoder->SetInputType(0, in_type.Get(), 0)
            : E_FAIL;
        if (FAILED(out_hr)) {
            HRESULT first_in = encoder->SetInputType(0, in_type.Get(), 0);
            if (SUCCEEDED(first_in)) {
                out_hr = encoder->SetOutputType(0, out_type.Get(), 0);
                in_hr = first_in;
            }
        }
        ok = SUCCEEDED(out_hr) && SUCCEEDED(in_hr);
    }

    for (UINT32 i = 0; i < activate_count; ++i) {
        if (activates[i]) activates[i]->Release();
    }
    CoTaskMemFree(activates);
    return ok;
}

static bool mf_has_audio_decoder(avb_audio_codec codec) {
    switch (codec) {
        case AVB_AUDIO_CODEC_AAC:
            return mf_has_mft(MFT_CATEGORY_AUDIO_DECODER, MFMediaType_Audio,
                              MFAudioFormat_AAC, false);
        case AVB_AUDIO_CODEC_MP3:
            return mf_has_mft(MFT_CATEGORY_AUDIO_DECODER, MFMediaType_Audio,
                              MFAudioFormat_MP3, false);
        case AVB_AUDIO_CODEC_OPUS:
            return mf_has_mft(MFT_CATEGORY_AUDIO_DECODER, MFMediaType_Audio,
                              mf_subtype_from_wave_tag(0x704f), false);
        case AVB_AUDIO_CODEC_FLAC:
            return mf_has_mft(MFT_CATEGORY_AUDIO_DECODER, MFMediaType_Audio,
                              mf_subtype_from_wave_tag(0xf1ac), false);
        case AVB_AUDIO_CODEC_VORBIS:
            return mf_has_mft(MFT_CATEGORY_AUDIO_DECODER, MFMediaType_Audio,
                              mf_vorbis_subtype(), false);
        case AVB_AUDIO_CODEC_PCM_S16:
            return mf_has_mft(MFT_CATEGORY_AUDIO_DECODER, MFMediaType_Audio,
                              MFAudioFormat_PCM, false);
        case AVB_AUDIO_CODEC_PCM_F32:
            return mf_has_mft(MFT_CATEGORY_AUDIO_DECODER, MFMediaType_Audio,
                              MFAudioFormat_Float, false);
        default:
            return false;
    }
}

static bool mf_has_audio_encoder(avb_audio_codec codec) {
    switch (codec) {
        case AVB_AUDIO_CODEC_AAC:
            return mf_has_mft(MFT_CATEGORY_AUDIO_ENCODER, MFMediaType_Audio,
                              MFAudioFormat_AAC, true);
        case AVB_AUDIO_CODEC_MP3:
            return mf_has_mft(MFT_CATEGORY_AUDIO_ENCODER, MFMediaType_Audio,
                              MFAudioFormat_MP3, true);
        case AVB_AUDIO_CODEC_FLAC:
            return mf_has_mft(MFT_CATEGORY_AUDIO_ENCODER, MFMediaType_Audio,
                              MFAudioFormat_FLAC, true);
        case AVB_AUDIO_CODEC_PCM_S16:
            return mf_has_mft(MFT_CATEGORY_AUDIO_ENCODER, MFMediaType_Audio,
                              MFAudioFormat_PCM, true);
        case AVB_AUDIO_CODEC_PCM_F32:
            return mf_has_mft(MFT_CATEGORY_AUDIO_ENCODER, MFMediaType_Audio,
                              MFAudioFormat_Float, true);
        default:
            return false;
    }
}

static void fill_mediafoundation_decoder_runtime(avb_decoder_capabilities &out,
                                                 Container c) {
    if (c == Container::ivf) {
        static const avb_video_codec ivf_video[] = {
            AVB_VIDEO_CODEC_VP8, AVB_VIDEO_CODEC_VP9, AVB_VIDEO_CODEC_AV1
        };
        for (avb_video_codec codec : ivf_video) {
            if (mf_has_video_decoder(codec)) add_video_codec(out, codec, c);
        }
        add_software_pixel_formats(out);
        add_memory(out, AVB_VIDEO_MEMORY_CPU);
        add_memory(out, AVB_VIDEO_MEMORY_NATIVE);
        add_device(out, AVB_HW_DEVICE_AUTO);
        add_device(out, AVB_HW_DEVICE_D3D11VA);
        return;
    }
    static const avb_video_codec video[] = {
        AVB_VIDEO_CODEC_H264, AVB_VIDEO_CODEC_HEVC, AVB_VIDEO_CODEC_VP8,
        AVB_VIDEO_CODEC_VP9, AVB_VIDEO_CODEC_AV1
    };
    static const avb_audio_codec audio[] = {
        AVB_AUDIO_CODEC_AAC, AVB_AUDIO_CODEC_OPUS, AVB_AUDIO_CODEC_MP3,
        AVB_AUDIO_CODEC_FLAC, AVB_AUDIO_CODEC_VORBIS,
        AVB_AUDIO_CODEC_PCM_S16, AVB_AUDIO_CODEC_PCM_F32
    };
    if (!audio_only_container(c)) {
        for (avb_video_codec codec : video) {
            if (mf_has_video_decoder(codec)) add_video_codec(out, codec, c);
        }
    }
    for (avb_audio_codec codec : audio) {
        if (codec == AVB_AUDIO_CODEC_PCM_S16 ||
            codec == AVB_AUDIO_CODEC_PCM_F32 ||
            mf_has_audio_decoder(codec)) {
            add_audio_codec(out, codec, c);
        }
    }

    add_software_pixel_formats(out);
    add_memory(out, AVB_VIDEO_MEMORY_CPU);
    if (!audio_only_container(c))
        add_memory(out, AVB_VIDEO_MEMORY_NATIVE);
    add_device(out, AVB_HW_DEVICE_AUTO);
    if (!audio_only_container(c))
        add_device(out, AVB_HW_DEVICE_D3D11VA);
}

static void fill_mediafoundation_encoder_runtime(avb_encoder_capabilities &out,
                                                 Container c) {
    // VP8/VP9/AV1 are exposed for IVF only; WebM/Matroska still need a separate
    // muxing path. Async transforms are driven through IMFMediaEventGenerator.
    static const avb_video_codec video[] = {
        AVB_VIDEO_CODEC_H264, AVB_VIDEO_CODEC_HEVC, AVB_VIDEO_CODEC_VP8,
        AVB_VIDEO_CODEC_VP9, AVB_VIDEO_CODEC_AV1
    };
    if (mediafoundation_video_container(c)) {
        for (avb_video_codec codec : video) {
            if (codec == AVB_VIDEO_CODEC_AV1 &&
                c != Container::any && c != Container::ivf) {
                continue;
            }
            if (mf_can_configure_video_encoder(codec)) add_video_codec(out, codec, c);
        }
    }

    switch (c) {
        case Container::any:
            if (mf_has_audio_encoder(AVB_AUDIO_CODEC_AAC))
                add_audio_codec_unchecked(out, AVB_AUDIO_CODEC_AAC);
            if (mf_has_audio_encoder(AVB_AUDIO_CODEC_MP3))
                add_audio_codec_unchecked(out, AVB_AUDIO_CODEC_MP3);
            if (mf_has_audio_encoder(AVB_AUDIO_CODEC_FLAC))
                add_audio_codec_unchecked(out, AVB_AUDIO_CODEC_FLAC);
            add_audio_codec_unchecked(out, AVB_AUDIO_CODEC_PCM_S16);
            add_audio_codec_unchecked(out, AVB_AUDIO_CODEC_PCM_F32);
            break;
        case Container::mp4:
        case Container::mov:
        case Container::m4a:
        case Container::unknown:
            if (mf_has_audio_encoder(AVB_AUDIO_CODEC_AAC))
                add_audio_codec_unchecked(out, AVB_AUDIO_CODEC_AAC);
            break;
        case Container::mp3:
            if (mf_has_audio_encoder(AVB_AUDIO_CODEC_MP3))
                add_audio_codec_unchecked(out, AVB_AUDIO_CODEC_MP3);
            break;
        case Container::flac:
            if (mf_has_audio_encoder(AVB_AUDIO_CODEC_FLAC))
                add_audio_codec_unchecked(out, AVB_AUDIO_CODEC_FLAC);
            break;
        case Container::wav:
            add_audio_codec_unchecked(out, AVB_AUDIO_CODEC_PCM_S16);
            add_audio_codec_unchecked(out, AVB_AUDIO_CODEC_PCM_F32);
            break;
        default:
            break;
    }

    add_memory(out, AVB_VIDEO_MEMORY_CPU);
    if (c == Container::any || c == Container::ivf ||
        c == Container::mp4 || c == Container::mov)
        add_memory(out, AVB_VIDEO_MEMORY_NATIVE);
    add_device(out, AVB_HW_DEVICE_AUTO);
    if (c == Container::any || c == Container::ivf ||
        c == Container::mp4 || c == Container::mov)
        add_device(out, AVB_HW_DEVICE_D3D11VA);
}
#endif

#if defined(AVB_ENABLE_FFMPEG)
static AVCodecID ff_video_codec_id(avb_video_codec codec) {
    switch (codec) {
        case AVB_VIDEO_CODEC_H264: return AV_CODEC_ID_H264;
        case AVB_VIDEO_CODEC_HEVC: return AV_CODEC_ID_HEVC;
        case AVB_VIDEO_CODEC_VP8:  return AV_CODEC_ID_VP8;
        case AVB_VIDEO_CODEC_VP9:  return AV_CODEC_ID_VP9;
        case AVB_VIDEO_CODEC_AV1:  return AV_CODEC_ID_AV1;
        default: return AV_CODEC_ID_NONE;
    }
}

static AVCodecID ff_audio_codec_id(avb_audio_codec codec) {
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

static AVHWDeviceType ff_hw_type(avb_hardware_device device) {
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

static bool ff_device_available(const AvbFFmpegFuncs &ff, avb_hardware_device device) {
    AVHWDeviceType type = ff_hw_type(device);
    if (type == AV_HWDEVICE_TYPE_NONE) return false;
    AVBufferRef *ctx = nullptr;
    int ret = ff.av_hwdevice_ctx_create(&ctx, type, nullptr, nullptr, 0);
    if (ctx) ff.av_buffer_unref(&ctx);
    return ret >= 0;
}

static const char *const *ff_hw_encoder_names(avb_video_codec codec,
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

static bool ff_has_hw_encoder(const AvbFFmpegFuncs &ff, avb_video_codec codec,
                              avb_hardware_device device) {
    if (!ff_device_available(ff, device)) return false;
    const char *const *names = ff_hw_encoder_names(codec, device);
    for (int i = 0; names[i]; ++i) {
        if (ff.avcodec_find_encoder_by_name(names[i])) return true;
    }
    return false;
}

static bool ff_has_hw_decoder(const AvbFFmpegFuncs &ff, avb_video_codec codec,
                              avb_hardware_device device) {
    const AVCodec *decoder = ff.avcodec_find_decoder(ff_video_codec_id(codec));
    if (!decoder || !ff_device_available(ff, device)) return false;
    AVHWDeviceType type = ff_hw_type(device);
    for (int i = 0;; ++i) {
        const AVCodecHWConfig *cfg = ff.avcodec_get_hw_config(decoder, i);
        if (!cfg) break;
        if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
            cfg->device_type == type) {
            return true;
        }
    }
    return false;
}

static bool ff_has_video_encoder(const AvbFFmpegFuncs &ff, avb_video_codec codec) {
    AVCodecID id = ff_video_codec_id(codec);
    return (id != AV_CODEC_ID_NONE && ff.avcodec_find_encoder(id)) ||
           ff_has_hw_encoder(ff, codec, AVB_HW_DEVICE_VAAPI);
}

static bool ff_has_audio_encoder(const AvbFFmpegFuncs &ff, avb_audio_codec codec) {
    const char *preferred = nullptr;
    switch (codec) {
        case AVB_AUDIO_CODEC_OPUS:   preferred = "libopus"; break;
        case AVB_AUDIO_CODEC_MP3:    preferred = "libmp3lame"; break;
        case AVB_AUDIO_CODEC_VORBIS: preferred = "libvorbis"; break;
        default: break;
    }
    if (preferred && ff.avcodec_find_encoder_by_name(preferred)) return true;
    AVCodecID id = ff_audio_codec_id(codec);
    return id != AV_CODEC_ID_NONE && ff.avcodec_find_encoder(id);
}

static void fill_ffmpeg_decoder_runtime(avb_decoder_capabilities &out, Container c,
                                        const AvbFFmpegFuncs &ff) {
    const avb_video_codec video[] = {
        AVB_VIDEO_CODEC_H264, AVB_VIDEO_CODEC_HEVC, AVB_VIDEO_CODEC_VP8,
        AVB_VIDEO_CODEC_VP9, AVB_VIDEO_CODEC_AV1
    };
    const avb_audio_codec audio[] = {
        AVB_AUDIO_CODEC_AAC, AVB_AUDIO_CODEC_OPUS, AVB_AUDIO_CODEC_MP3,
        AVB_AUDIO_CODEC_FLAC, AVB_AUDIO_CODEC_VORBIS,
        AVB_AUDIO_CODEC_PCM_S16, AVB_AUDIO_CODEC_PCM_F32
    };
    if (!audio_only_container(c)) {
        for (avb_video_codec codec : video) {
            if (ff.avcodec_find_decoder(ff_video_codec_id(codec)))
                add_video_codec(out, codec, c);
        }
    }
    for (avb_audio_codec codec : audio) {
        if (ff.avcodec_find_decoder(ff_audio_codec_id(codec)))
            add_audio_codec(out, codec, c);
    }

    add_software_pixel_formats(out);
    add_memory(out, AVB_VIDEO_MEMORY_CPU);
    add_device(out, AVB_HW_DEVICE_AUTO);
    bool has_hw = false;
    if (ff_device_available(ff, AVB_HW_DEVICE_VAAPI)) {
        add_device(out, AVB_HW_DEVICE_VAAPI);
        has_hw = true;
    }
    for (avb_video_codec codec : video) {
        if (!has_hw && ff_has_hw_decoder(ff, codec, AVB_HW_DEVICE_VAAPI))
            has_hw = true;
    }
    if (has_hw) {
        add_memory(out, AVB_VIDEO_MEMORY_NATIVE);
#if defined(__linux__)
        add_memory(out, AVB_VIDEO_MEMORY_DMABUF);
#endif
    }
}

static void fill_ffmpeg_encoder_runtime(avb_encoder_capabilities &out, Container c,
                                        const AvbFFmpegFuncs &ff) {
    const avb_video_codec video[] = {
        AVB_VIDEO_CODEC_H264, AVB_VIDEO_CODEC_HEVC, AVB_VIDEO_CODEC_VP8,
        AVB_VIDEO_CODEC_VP9, AVB_VIDEO_CODEC_AV1
    };
    const avb_audio_codec audio[] = {
        AVB_AUDIO_CODEC_AAC, AVB_AUDIO_CODEC_OPUS, AVB_AUDIO_CODEC_MP3,
        AVB_AUDIO_CODEC_FLAC, AVB_AUDIO_CODEC_VORBIS,
        AVB_AUDIO_CODEC_PCM_S16, AVB_AUDIO_CODEC_PCM_F32
    };
    if (!audio_only_container(c)) {
        for (avb_video_codec codec : video) {
            if (ff_has_video_encoder(ff, codec)) add_video_codec(out, codec, c);
        }
    }
    for (avb_audio_codec codec : audio) {
        if (ff_has_audio_encoder(ff, codec)) add_audio_codec(out, codec, c);
    }

    add_memory(out, AVB_VIDEO_MEMORY_CPU);
    add_device(out, AVB_HW_DEVICE_AUTO);
    if (ff_device_available(ff, AVB_HW_DEVICE_VAAPI)) {
        add_device(out, AVB_HW_DEVICE_VAAPI);
        add_memory(out, AVB_VIDEO_MEMORY_NATIVE);
#if defined(__linux__)
        add_memory(out, AVB_VIDEO_MEMORY_DMABUF);
#endif
    }
}
#endif

#if defined(AVB_ENABLE_GSTREAMER)
static bool gst_has_element(const AvbGstFuncs &gst, const char *name) {
    GstElement *element = gst.gst_element_factory_make(name, nullptr);
    if (!element) return false;
    gst.gst_object_unref(element);
    return true;
}

static bool gst_has_any(const AvbGstFuncs &gst, const char *const *names) {
    for (int i = 0; names[i]; ++i) {
        if (gst_has_element(gst, names[i])) return true;
    }
    return false;
}

static bool gst_has_vaapi(const AvbGstFuncs &gst) {
    static const char *const va[] = {
        "vapostproc", "vah264enc", "vah265enc", "vah264dec", "vah265dec",
        "vavp9lpenc", "vavp9dec", nullptr
    };
    return gst_has_any(gst, va);
}

static bool gst_has_video_decoder(const AvbGstFuncs &gst, avb_video_codec codec) {
    static const char *const h264[] = {"avdec_h264", "openh264dec", "vah264dec", nullptr};
    static const char *const hevc[] = {"avdec_h265", "vah265dec", "libde265dec", nullptr};
    static const char *const vp8[] = {"vp8dec", "avdec_vp8", nullptr};
    static const char *const vp9[] = {"vp9dec", "avdec_vp9", "vavp9dec", nullptr};
    static const char *const av1[] = {"av1dec", "dav1ddec", "avdec_av1", nullptr};
    switch (codec) {
        case AVB_VIDEO_CODEC_H264: return gst_has_any(gst, h264);
        case AVB_VIDEO_CODEC_HEVC: return gst_has_any(gst, hevc);
        case AVB_VIDEO_CODEC_VP8:  return gst_has_any(gst, vp8);
        case AVB_VIDEO_CODEC_VP9:  return gst_has_any(gst, vp9);
        case AVB_VIDEO_CODEC_AV1:  return gst_has_any(gst, av1);
        default: return false;
    }
}

static bool gst_has_video_encoder(const AvbGstFuncs &gst, avb_video_codec codec) {
    static const char *const h264[] = {"x264enc", "openh264enc", "vah264enc", nullptr};
    static const char *const hevc[] = {"x265enc", "vah265enc", nullptr};
    static const char *const vp8[] = {"vp8enc", nullptr};
    static const char *const vp9[] = {"vp9enc", "vavp9lpenc", nullptr};
    static const char *const av1[] = {"av1enc", nullptr};
    switch (codec) {
        case AVB_VIDEO_CODEC_H264: return gst_has_any(gst, h264);
        case AVB_VIDEO_CODEC_HEVC: return gst_has_any(gst, hevc);
        case AVB_VIDEO_CODEC_VP8:  return gst_has_any(gst, vp8);
        case AVB_VIDEO_CODEC_VP9:  return gst_has_any(gst, vp9);
        case AVB_VIDEO_CODEC_AV1:  return gst_has_any(gst, av1);
        default: return false;
    }
}

static bool gst_has_audio_decoder(const AvbGstFuncs &gst, avb_audio_codec codec) {
    static const char *const aac[] = {"avdec_aac", "faad", nullptr};
    static const char *const opus[] = {"opusdec", nullptr};
    static const char *const mp3[] = {"mpg123audiodec", "avdec_mp3float", "avdec_mp3", nullptr};
    static const char *const flac[] = {"flacdec", nullptr};
    static const char *const vorbis[] = {"vorbisdec", nullptr};
    switch (codec) {
        case AVB_AUDIO_CODEC_AAC:     return gst_has_any(gst, aac);
        case AVB_AUDIO_CODEC_OPUS:    return gst_has_any(gst, opus);
        case AVB_AUDIO_CODEC_MP3:     return gst_has_any(gst, mp3);
        case AVB_AUDIO_CODEC_FLAC:    return gst_has_any(gst, flac);
        case AVB_AUDIO_CODEC_VORBIS:  return gst_has_any(gst, vorbis);
        case AVB_AUDIO_CODEC_PCM_S16:
        case AVB_AUDIO_CODEC_PCM_F32: return gst_has_element(gst, "audioconvert");
        default: return false;
    }
}

static bool gst_has_audio_encoder(const AvbGstFuncs &gst, avb_audio_codec codec,
                                  Container c) {
    switch (codec) {
        case AVB_AUDIO_CODEC_AAC:
            return gst_has_element(gst, "avenc_aac");
        case AVB_AUDIO_CODEC_OPUS:
            return gst_has_element(gst, "opusenc");
        case AVB_AUDIO_CODEC_MP3:
            return c == Container::mp3 ? gst_has_element(gst, "lamemp3enc") :
                                         gst_has_element(gst, "lamemp3enc");
        case AVB_AUDIO_CODEC_FLAC:
            return c == Container::flac ? gst_has_element(gst, "flacenc") :
                                          gst_has_element(gst, "flacenc");
        case AVB_AUDIO_CODEC_VORBIS:
            return gst_has_element(gst, "vorbisenc");
        case AVB_AUDIO_CODEC_PCM_S16:
        case AVB_AUDIO_CODEC_PCM_F32:
            return c == Container::any || c == Container::wav
                ? gst_has_element(gst, "wavenc") : false;
        default:
            return false;
    }
}

static bool gst_has_dmabuf_allocator(const AvbGstFuncs &gst) {
    GstAllocator *allocator = gst.gst_dmabuf_allocator_new();
    if (!allocator) return false;
    gst.gst_object_unref(allocator);
    return true;
}

static void fill_gstreamer_decoder_runtime(avb_decoder_capabilities &out,
                                           Container c,
                                           const AvbGstFuncs &gst) {
    const avb_video_codec video[] = {
        AVB_VIDEO_CODEC_H264, AVB_VIDEO_CODEC_HEVC, AVB_VIDEO_CODEC_VP8,
        AVB_VIDEO_CODEC_VP9, AVB_VIDEO_CODEC_AV1
    };
    const avb_audio_codec audio[] = {
        AVB_AUDIO_CODEC_AAC, AVB_AUDIO_CODEC_OPUS, AVB_AUDIO_CODEC_MP3,
        AVB_AUDIO_CODEC_FLAC, AVB_AUDIO_CODEC_VORBIS,
        AVB_AUDIO_CODEC_PCM_S16, AVB_AUDIO_CODEC_PCM_F32
    };
    if (!audio_only_container(c)) {
        for (avb_video_codec codec : video) {
            if (gst_has_video_decoder(gst, codec)) add_video_codec(out, codec, c);
        }
    }
    for (avb_audio_codec codec : audio) {
        if (gst_has_audio_decoder(gst, codec)) add_audio_codec(out, codec, c);
    }
    add_software_pixel_formats(out);
    add_memory(out, AVB_VIDEO_MEMORY_CPU);
    add_memory(out, AVB_VIDEO_MEMORY_NATIVE);
    add_device(out, AVB_HW_DEVICE_AUTO);
    if (gst_has_vaapi(gst)) {
        add_device(out, AVB_HW_DEVICE_VAAPI);
        if (gst_has_dmabuf_allocator(gst)) add_memory(out, AVB_VIDEO_MEMORY_DMABUF);
    }
}

static void fill_gstreamer_encoder_runtime(avb_encoder_capabilities &out,
                                           Container c,
                                           const AvbGstFuncs &gst) {
    const avb_video_codec video[] = {
        AVB_VIDEO_CODEC_H264, AVB_VIDEO_CODEC_HEVC, AVB_VIDEO_CODEC_VP8,
        AVB_VIDEO_CODEC_VP9, AVB_VIDEO_CODEC_AV1
    };
    const avb_audio_codec audio[] = {
        AVB_AUDIO_CODEC_AAC, AVB_AUDIO_CODEC_OPUS, AVB_AUDIO_CODEC_MP3,
        AVB_AUDIO_CODEC_FLAC, AVB_AUDIO_CODEC_VORBIS,
        AVB_AUDIO_CODEC_PCM_S16, AVB_AUDIO_CODEC_PCM_F32
    };
    if (!audio_only_container(c)) {
        for (avb_video_codec codec : video) {
            if (gst_has_video_encoder(gst, codec)) add_video_codec(out, codec, c);
        }
    }
    for (avb_audio_codec codec : audio) {
        if (gst_has_audio_encoder(gst, codec, c)) add_audio_codec(out, codec, c);
    }
    add_memory(out, AVB_VIDEO_MEMORY_CPU);
    add_device(out, AVB_HW_DEVICE_AUTO);
    if (gst_has_vaapi(gst)) {
        add_device(out, AVB_HW_DEVICE_VAAPI);
        add_memory(out, AVB_VIDEO_MEMORY_NATIVE);
        if (gst_has_dmabuf_allocator(gst)) add_memory(out, AVB_VIDEO_MEMORY_DMABUF);
    }
}
#endif

#if defined(AVB_ENABLE_AVFOUNDATION)
// AVFoundation runtime probing leans on VideoToolbox (VTCopyVideoEncoderList /
// VTIsHardwareDecodeSupported) and AudioToolbox (kAudioFormatProperty_Decoders /
// _Encoders) so the reported codecs reflect what this macOS runtime can really
// do instead of a static H.264/HEVC/AAC assumption. AVAssetReader/AVAssetWriter
// can still reject a concrete session over container, profile, or memory limits.
static bool vt_has_audio_codec(UInt32 format_id, bool encoder) {
    if (format_id == 0) return false;
    UInt32 size = 0;
    OSStatus st = AudioFormatGetPropertyInfo(
        encoder ? kAudioFormatProperty_Encoders : kAudioFormatProperty_Decoders,
        sizeof(UInt32), &format_id, &size);
    return st == noErr && size > 0;
}

static UInt32 vt_audio_format_id(avb_audio_codec codec) {
    switch (codec) {
        case AVB_AUDIO_CODEC_AAC:  return kAudioFormatMPEG4AAC;
        case AVB_AUDIO_CODEC_MP3:  return kAudioFormatMPEGLayer3;
        case AVB_AUDIO_CODEC_FLAC: return kAudioFormatFLAC;
        case AVB_AUDIO_CODEC_OPUS: return kAudioFormatOpus;
        default:                   return 0;
    }
}

static CMVideoCodecType vt_video_codec_type(avb_video_codec codec) {
    switch (codec) {
        case AVB_VIDEO_CODEC_H264: return kCMVideoCodecType_H264;
        case AVB_VIDEO_CODEC_HEVC: return kCMVideoCodecType_HEVC;
        case AVB_VIDEO_CODEC_VP9:  return (CMVideoCodecType)'vp09';
        case AVB_VIDEO_CODEC_AV1:  return (CMVideoCodecType)'av01';
        default:                   return 0;
    }
}

static bool vt_has_video_encoder(CMVideoCodecType type) {
    if (type == 0) return false;
    CFArrayRef list = nullptr;
    if (VTCopyVideoEncoderList(nullptr, &list) != noErr || !list) return false;
    bool found = false;
    CFIndex count = CFArrayGetCount(list);
    for (CFIndex i = 0; i < count && !found; ++i) {
        CFDictionaryRef entry = (CFDictionaryRef)CFArrayGetValueAtIndex(list, i);
        CFNumberRef codec_type = (CFNumberRef)CFDictionaryGetValue(
            entry, kVTVideoEncoderList_CodecType);
        int32_t value = 0;
        if (codec_type &&
            CFNumberGetValue(codec_type, kCFNumberSInt32Type, &value) &&
            (CMVideoCodecType)value == type) {
            found = true;
        }
    }
    CFRelease(list);
    return found;
}

static bool vt_can_decode(avb_video_codec codec) {
    switch (codec) {
        // VideoToolbox always provides H.264/HEVC decode on macOS.
        case AVB_VIDEO_CODEC_H264:
        case AVB_VIDEO_CODEC_HEVC:
            return true;
        // VP9/AV1 decode is only present when the runtime advertises hardware
        // support (Apple silicon); AVFoundation has no software fallback for them.
        case AVB_VIDEO_CODEC_VP9:
        case AVB_VIDEO_CODEC_AV1:
            return VTIsHardwareDecodeSupported(vt_video_codec_type(codec));
        default:
            return false;
    }
}

static void fill_avfoundation_decoder_runtime(avb_decoder_capabilities &out,
                                              Container c) {
    static const avb_video_codec video[] = {
        AVB_VIDEO_CODEC_H264, AVB_VIDEO_CODEC_HEVC,
        AVB_VIDEO_CODEC_VP9, AVB_VIDEO_CODEC_AV1
    };
    static const avb_audio_codec audio[] = {
        AVB_AUDIO_CODEC_AAC, AVB_AUDIO_CODEC_MP3, AVB_AUDIO_CODEC_FLAC,
        AVB_AUDIO_CODEC_OPUS, AVB_AUDIO_CODEC_PCM_S16, AVB_AUDIO_CODEC_PCM_F32
    };
    if (!audio_only_container(c)) {
        for (avb_video_codec codec : video) {
            if (vt_can_decode(codec)) add_video_codec(out, codec, c);
        }
    }
    for (avb_audio_codec codec : audio) {
        if (codec == AVB_AUDIO_CODEC_PCM_S16 ||
            codec == AVB_AUDIO_CODEC_PCM_F32 ||
            vt_has_audio_codec(vt_audio_format_id(codec), false)) {
            add_audio_codec(out, codec, c);
        }
    }
    add_software_pixel_formats(out);
    add_memory(out, AVB_VIDEO_MEMORY_CPU);
    if (!audio_only_container(c))
        add_memory(out, AVB_VIDEO_MEMORY_NATIVE);
    add_device(out, AVB_HW_DEVICE_AUTO);
    add_device(out, AVB_HW_DEVICE_VIDEOTOOLBOX);
}

static void fill_avfoundation_encoder_runtime(avb_encoder_capabilities &out,
                                              Container c) {
    static const avb_video_codec video[] = {
        AVB_VIDEO_CODEC_H264, AVB_VIDEO_CODEC_HEVC
    };
    if (!audio_only_container(c)) {
        for (avb_video_codec codec : video) {
            if (vt_has_video_encoder(vt_video_codec_type(codec)))
                add_video_codec(out, codec, c);
        }
    }
    // AVAssetWriter only produces AAC built-in audio across its containers.
    if (vt_has_audio_codec(kAudioFormatMPEG4AAC, true))
        add_audio_codec(out, AVB_AUDIO_CODEC_AAC, c);

    add_memory(out, AVB_VIDEO_MEMORY_CPU);
    if (!audio_only_container(c))
        add_memory(out, AVB_VIDEO_MEMORY_NATIVE);
    add_device(out, AVB_HW_DEVICE_AUTO);
    add_device(out, AVB_HW_DEVICE_VIDEOTOOLBOX);
}
#endif

} // namespace

extern "C" {

avb_result avb_decoder_probe_runtime_capabilities(
    avb_backend backend,
    const char *path,
    avb_decoder_capabilities *out
) {
    if (!out) return AVB_ERROR_INVALID_ARGUMENT;

    *out = {};
    Container container = container_from_path(path, Container::any);
    out->result = AVB_OK;
    out->backend = resolve_backend(backend);
    out->backend_name = avb_backend_name(out->backend);
    out->container_name = container_name(container);

    if (!out->backend_name || !avb_backend_is_available(out->backend)) {
        out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
        out->message = "Requested decoder backend is not available in this build.";
        return AVB_OK;
    }

    switch (out->backend) {
        case AVB_BACKEND_FFMPEG:
#if defined(AVB_ENABLE_FFMPEG)
        {
            AvbFFmpegFuncs ff{};
            char err_buf[AVB_MAX_ERROR] = {};
            if (!avb_ffmpeg_load(ff, err_buf, sizeof(err_buf))) {
                out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
                out->message = "FFmpeg runtime libraries are not available.";
                return AVB_OK;
            }
            fill_ffmpeg_decoder_runtime(*out, container, ff);
            break;
        }
#else
            out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
            out->message = "FFmpeg backend is not built.";
            return AVB_OK;
#endif

        case AVB_BACKEND_GSTREAMER:
#if defined(AVB_ENABLE_GSTREAMER)
        {
            AvbGstFuncs gst{};
            char err_buf[AVB_MAX_ERROR] = {};
            if (!avb_gst_load(gst, err_buf, sizeof(err_buf))) {
                out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
                out->message = "GStreamer runtime libraries are not available.";
                return AVB_OK;
            }
            gst.gst_init(nullptr, nullptr);
            fill_gstreamer_decoder_runtime(*out, container, gst);
            break;
        }
#else
            out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
            out->message = "GStreamer backend is not built.";
            return AVB_OK;
#endif

        case AVB_BACKEND_MEDIAFOUNDATION:
#if defined(AVB_ENABLE_MEDIAFOUNDATION)
        {
            MfStartupScope mf;
            if (!mf.started) {
                out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
                out->message = "Media Foundation runtime is not available.";
                return AVB_OK;
            }
            fill_mediafoundation_decoder_runtime(*out, container);
            break;
        }
#else
            out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
            out->message = "Media Foundation backend is not built.";
            return AVB_OK;
#endif

        case AVB_BACKEND_AVFOUNDATION:
#if defined(AVB_ENABLE_AVFOUNDATION)
            fill_avfoundation_decoder_runtime(*out, container);
#else
            fill_platform_decoder(*out, container);
#endif
            break;

        default:
            out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
            out->message = "Requested decoder backend is not available.";
            return AVB_OK;
    }

    out->can_decode_video = out->video_codec_count > 0 ? 1 : 0;
    out->can_decode_audio = out->audio_codec_count > 0 ? 1 : 0;
    out->message = "Decoder capabilities are available in the current runtime.";
    return AVB_OK;
}

avb_result avb_encoder_probe_runtime_capabilities(
    avb_backend backend,
    const char *path,
    avb_encoder_capabilities *out
) {
    if (!out) return AVB_ERROR_INVALID_ARGUMENT;

    *out = {};
    Container container = container_from_path(path, Container::any);
    out->result = AVB_OK;
    out->backend = resolve_backend(backend);
    out->backend_name = avb_backend_name(out->backend);
    out->container_name = container_name(container);

    if (!out->backend_name || !avb_backend_is_available(out->backend)) {
        out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
        out->message = "Requested encoder backend is not available in this build.";
        return AVB_OK;
    }

    switch (out->backend) {
        case AVB_BACKEND_FFMPEG:
#if defined(AVB_ENABLE_FFMPEG)
        {
            AvbFFmpegFuncs ff{};
            char err_buf[AVB_MAX_ERROR] = {};
            if (!avb_ffmpeg_load(ff, err_buf, sizeof(err_buf))) {
                out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
                out->message = "FFmpeg runtime libraries are not available.";
                return AVB_OK;
            }
            fill_ffmpeg_encoder_runtime(*out, container, ff);
            break;
        }
#else
            out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
            out->message = "FFmpeg backend is not built.";
            return AVB_OK;
#endif

        case AVB_BACKEND_GSTREAMER:
#if defined(AVB_ENABLE_GSTREAMER)
        {
            AvbGstFuncs gst{};
            char err_buf[AVB_MAX_ERROR] = {};
            if (!avb_gst_load(gst, err_buf, sizeof(err_buf))) {
                out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
                out->message = "GStreamer runtime libraries are not available.";
                return AVB_OK;
            }
            gst.gst_init(nullptr, nullptr);
            fill_gstreamer_encoder_runtime(*out, container, gst);
            break;
        }
#else
            out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
            out->message = "GStreamer backend is not built.";
            return AVB_OK;
#endif

        case AVB_BACKEND_MEDIAFOUNDATION:
#if defined(AVB_ENABLE_MEDIAFOUNDATION)
        {
            MfStartupScope mf;
            if (!mf.started) {
                out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
                out->message = "Media Foundation runtime is not available.";
                return AVB_OK;
            }
            fill_mediafoundation_encoder_runtime(*out, container);
            break;
        }
#else
            out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
            out->message = "Media Foundation backend is not built.";
            return AVB_OK;
#endif

        case AVB_BACKEND_AVFOUNDATION:
#if defined(AVB_ENABLE_AVFOUNDATION)
            fill_avfoundation_encoder_runtime(*out, container);
#else
            fill_platform_encoder(*out, container);
#endif
            break;

        default:
            out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
            out->message = "Requested encoder backend is not available.";
            return AVB_OK;
    }

    out->can_encode_video = out->video_codec_count > 0 ? 1 : 0;
    out->can_encode_audio = out->audio_codec_count > 0 ? 1 : 0;
    out->message = "Encoder capabilities are available in the current runtime.";
    return AVB_OK;
}

} // extern "C"
