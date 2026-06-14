#include "avb_encoder_mediafoundation.hpp"
#include "../../avb_capability_common.hpp"
#include "../../avb_video_codec_registry.hpp"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX  // keep <windows.h> from defining min/max macros
#endif

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#include <mmreg.h>
#include <wmcodecdsp.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
using avb::detail::Container;
using avb::detail::container_from_path;

// MF works in 100-ns ("hns") units; convert seconds to that.
static inline LONGLONG sec_to_hns(double s) { return (LONGLONG)std::llround(s * 1e7); }

static int mf_mp3_bitrate(int requested) {
    static const int allowed[] = {
        320000, 256000, 224000, 192000, 160000, 128000, 112000,
        96000, 80000, 64000, 56000, 48000, 40000, 32000
    };
    int want = requested > 0 ? requested : 128000;
    int best = allowed[0];
    int best_delta = std::abs(want - best);
    for (int v : allowed) {
        int delta = std::abs(want - v);
        if (delta < best_delta) {
            best = v;
            best_delta = delta;
        }
    }
    return best;
}

struct AvbEncoderMediaFoundation::Impl {
    ComPtr<IMFSinkWriter> writer;
    ComPtr<ID3D11Device> d3d_device;
    ComPtr<IMFDXGIDeviceManager> d3d_device_manager;

    DWORD video_stream = 0;
    DWORD audio_stream = 0;

    bool   has_video = false;
    bool   has_audio = false;

    int              width = 0, height = 0;
    double           frame_rate = 30.0;
    UINT32           fps_num = 30, fps_den = 1;
    avb_pixel_format input_format = AVB_PIXEL_FORMAT_BGRA8;
    avb_video_memory_type input_memory = AVB_VIDEO_MEMORY_CPU;
    bool             video_is_nv12 = false;
    bool             video_is_i420 = false;
    bool             swizzle_rgba   = false; // RGBA8 input -> swizzle to BGRA for RGB32
    bool             custom_video = false;
    const avb_video_encoder_plugin *custom_video_encoder = nullptr;
    void            *custom_video_ctx = nullptr;
    avb_encoded_video_stream custom_video_stream{};
    bool             ivf_video = false;
    avb_video_codec  ivf_codec = AVB_VIDEO_CODEC_AUTO;
    FILE            *ivf_file = nullptr;
    ComPtr<IMFTransform> video_encoder;
    ComPtr<IMFMediaEventGenerator> video_event_generator;
    bool             video_encoder_async = false;
    DWORD            video_encoder_out_size = 0;
    DWORD            video_encoder_out_flags = 0;
    bool             ivf_mft_input_nv12 = true;
    uint32_t         ivf_frame_count = 0;

    int     sample_rate = 0;
    int     channels    = 0;
    avb_audio_codec audio_codec = AVB_AUDIO_CODEC_AUTO;
    bool    audio_encoded_by_mft = false;
    ComPtr<IMFTransform> audio_encoder;
    DWORD   audio_encoder_out_size = 0;

    long    video_index   = 0; // for derived PTS
    int64_t audio_samples  = 0; // running total for audio PTS

    bool    began    = false;
    bool    finished = false;

    bool    mf_initialized = false;

    std::vector<unsigned char> video_stage; // contiguous repack scratch
    std::vector<int16_t>       audio_stage; // float -> S16 scratch

    void close_custom_video() {
        if (custom_video_encoder && custom_video_encoder->close && custom_video_ctx)
            custom_video_encoder->close(custom_video_ctx);
        custom_video_encoder = nullptr;
        custom_video_ctx = nullptr;
        custom_video_stream = {};
        custom_video = false;
    }

    void reset_streams() {
        writer.Reset();
        d3d_device_manager.Reset();
        d3d_device.Reset();
        close_custom_video();
        video_encoder.Reset();
        video_event_generator.Reset();
        if (ivf_file) {
            fclose(ivf_file);
            ivf_file = nullptr;
        }
        video_stream = 0;
        audio_stream = 0;
        has_video = false;
        has_audio = false;
        width = height = 0;
        frame_rate = 30.0;
        fps_num = 30;
        fps_den = 1;
        input_format = AVB_PIXEL_FORMAT_BGRA8;
        input_memory = AVB_VIDEO_MEMORY_CPU;
        video_is_nv12 = false;
        video_is_i420 = false;
        swizzle_rgba = false;
        ivf_video = false;
        ivf_codec = AVB_VIDEO_CODEC_AUTO;
        video_encoder_async = false;
        video_encoder_out_size = 0;
        video_encoder_out_flags = 0;
        ivf_mft_input_nv12 = true;
        ivf_frame_count = 0;
        sample_rate = 0;
        channels = 0;
        audio_codec = AVB_AUDIO_CODEC_AUTO;
        audio_encoded_by_mft = false;
        audio_encoder.Reset();
        audio_encoder_out_size = 0;
        video_index = 0;
        audio_samples = 0;
        began = false;
        finished = false;
        video_stage.clear();
        audio_stage.clear();
    }
};

static HRESULT create_d3d11_device_manager(
    ID3D11Device *input_device,
    ID3D11Device **device, IMFDXGIDeviceManager **manager) {
    if (!device || !manager) return E_POINTER;
    *device = nullptr;
    *manager = nullptr;

    ComPtr<ID3D11Device> created_device = input_device;
    HRESULT hr = S_OK;
    if (!created_device) {
        D3D_FEATURE_LEVEL feature_level{};
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
            D3D11_SDK_VERSION, &created_device, &feature_level, nullptr);
        if (FAILED(hr)) return hr;
    }

    ComPtr<IMFDXGIDeviceManager> created_manager;
    UINT reset_token = 0;
    hr = MFCreateDXGIDeviceManager(&reset_token, &created_manager);
    if (SUCCEEDED(hr))
        hr = created_manager->ResetDevice(created_device.Get(), reset_token);
    if (FAILED(hr)) return hr;

    hr = created_device.CopyTo(device);
    if (SUCCEEDED(hr)) hr = created_manager.CopyTo(manager);
    return hr;
}

AvbEncoderMediaFoundation::AvbEncoderMediaFoundation() {
    m_impl = new Impl();
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (SUCCEEDED(hr)) {
        m_impl->mf_initialized = true;
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "MFStartup failed: 0x%08lx", hr);
        m_last_error = buf;
    }
}

AvbEncoderMediaFoundation::~AvbEncoderMediaFoundation() {
    if (m_impl) {
        m_impl->close_custom_video();
        m_impl->writer.Reset();
        m_impl->video_event_generator.Reset();
        m_impl->video_encoder.Reset();
        if (m_impl->ivf_file) fclose(m_impl->ivf_file);
        if (m_impl->mf_initialized) MFShutdown();
        delete m_impl;
    }
}

const char *AvbEncoderMediaFoundation::get_last_error() const {
    return m_last_error.empty() ? nullptr : m_last_error.c_str();
}

// The Microsoft AAC encoder accepts only a fixed set of average byte rates.
// Snap the requested bits/sec to the nearest supported value.
static UINT32 aac_bytes_per_sec(int bitrate_bps) {
    const UINT32 allowed[] = { 12000, 16000, 20000, 24000 }; // 96/128/160/192 kbps
    UINT32 want = bitrate_bps > 0 ? (UINT32)(bitrate_bps / 8) : 16000;
    UINT32 best = allowed[0];
    UINT32 best_d = (want > best) ? want - best : best - want;
    for (UINT32 v : allowed) {
        UINT32 d = (want > v) ? want - v : v - want;
        if (d < best_d) { best = v; best_d = d; }
    }
    return best;
}

static HRESULT init_mp3_media_type(IMFMediaType *type, int sample_rate,
                                   int channels, int bitrate_bps) {
    if (!type) return E_POINTER;

    MPEGLAYER3WAVEFORMAT mp3{};
    mp3.wfx.wFormatTag = WAVE_FORMAT_MPEGLAYER3;
    mp3.wfx.nChannels = (WORD)channels;
    mp3.wfx.nSamplesPerSec = (DWORD)sample_rate;
    mp3.wfx.nAvgBytesPerSec = (DWORD)(bitrate_bps / 8);
    mp3.wfx.nBlockAlign = 1;
    mp3.wfx.wBitsPerSample = 0;
    mp3.wfx.cbSize = MPEGLAYER3_WFX_EXTRA_BYTES;
    mp3.wID = MPEGLAYER3_ID_MPEG;
    mp3.fdwFlags = MPEGLAYER3_FLAG_PADDING_ISO;
    mp3.nBlockSize = 1;
    mp3.nFramesPerBlock = 1;
    mp3.nCodecDelay = 0;
    return MFInitMediaTypeFromWaveFormatEx(type, &mp3.wfx, sizeof(mp3));
}

static HRESULT select_mp3_output_type(IMFTransform *encoder,
                                      IMFMediaType *preferred,
                                      UINT32 sample_rate,
                                      UINT32 channels,
                                      IMFMediaType **selected) {
    if (!encoder || !preferred || !selected) return E_POINTER;
    *selected = nullptr;

    HRESULT hr = encoder->SetOutputType(0, preferred, 0);
    if (SUCCEEDED(hr)) {
        preferred->AddRef();
        *selected = preferred;
        return hr;
    }

    for (DWORD i = 0; ; ++i) {
        ComPtr<IMFMediaType> candidate;
        HRESULT type_hr = encoder->GetOutputAvailableType(0, i, &candidate);
        if (type_hr == MF_E_NO_MORE_TYPES) break;
        if (FAILED(type_hr) || !candidate) continue;

        UINT32 sr = 0, ch = 0;
        candidate->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
        candidate->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
        if (sr != sample_rate || ch != channels) continue;

        type_hr = encoder->SetOutputType(0, candidate.Get(), 0);
        if (SUCCEEDED(type_hr)) {
            return candidate.CopyTo(selected);
        }
    }

    return hr;
}

static GUID mf_video_subtype_from_fourcc(uint32_t fcc) {
    GUID g = { fcc, 0x0000, 0x0010,
        { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
    return g;
}

static uint32_t mf_fourcc(const char (&s)[5]) {
    return ((uint32_t)(unsigned char)s[0]) |
           ((uint32_t)(unsigned char)s[1] << 8) |
           ((uint32_t)(unsigned char)s[2] << 16) |
           ((uint32_t)(unsigned char)s[3] << 24);
}

static GUID mf_video_subtype_from_codec(avb_video_codec codec, uint32_t codec_tag) {
    switch (codec) {
        case AVB_VIDEO_CODEC_H264: return MFVideoFormat_H264;
        case AVB_VIDEO_CODEC_HEVC: return MFVideoFormat_HEVC;
        case AVB_VIDEO_CODEC_VP8:  return MFVideoFormat_VP80;
        case AVB_VIDEO_CODEC_VP9:  return mf_video_subtype_from_fourcc(mf_fourcc("VP90"));
        case AVB_VIDEO_CODEC_AV1:  return MFVideoFormat_AV1;
        default:
            if (codec_tag == 0) return GUID_NULL;
            return mf_video_subtype_from_fourcc(codec_tag);
    }
}

static const char *mf_codec_name(avb_video_codec codec) {
    switch (codec) {
        case AVB_VIDEO_CODEC_H264: return "H264";
        case AVB_VIDEO_CODEC_HEVC: return "HEVC";
        case AVB_VIDEO_CODEC_VP8:  return "VP8";
        case AVB_VIDEO_CODEC_VP9:  return "VP9";
        case AVB_VIDEO_CODEC_AV1:  return "AV1";
        case AVB_VIDEO_CODEC_HAP:  return "HAP";
        default:             return "custom";
    }
}

static bool mf_ivf_codec(avb_video_codec codec) {
    return codec == AVB_VIDEO_CODEC_VP8 ||
           codec == AVB_VIDEO_CODEC_VP9 ||
           codec == AVB_VIDEO_CODEC_AV1;
}

static uint32_t ivf_fourcc(avb_video_codec codec) {
    switch (codec) {
        case AVB_VIDEO_CODEC_VP8: return mf_fourcc("VP80");
        case AVB_VIDEO_CODEC_VP9: return mf_fourcc("VP90");
        case AVB_VIDEO_CODEC_AV1: return mf_fourcc("AV01");
        default: return 0;
    }
}

static void put_le16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
}

static void put_le32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

static void put_le64(unsigned char *p, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = (unsigned char)((v >> (i * 8)) & 0xff);
    }
}

static bool write_ivf_header(FILE *f, avb_video_codec codec, int width, int height,
                             UINT32 fps_num, UINT32 fps_den, uint32_t frames) {
    if (!f) return false;
    unsigned char h[32] = {};
    h[0] = 'D'; h[1] = 'K'; h[2] = 'I'; h[3] = 'F';
    put_le16(h + 4, 0);
    put_le16(h + 6, 32);
    put_le32(h + 8, ivf_fourcc(codec));
    put_le16(h + 12, (uint16_t)width);
    put_le16(h + 14, (uint16_t)height);
    put_le32(h + 16, fps_num ? fps_num : 30);
    put_le32(h + 20, fps_den ? fps_den : 1);
    put_le32(h + 24, frames);
    put_le32(h + 28, 0);
    return fwrite(h, 1, sizeof(h), f) == sizeof(h);
}

static bool write_ivf_frame(FILE *f, const unsigned char *data, DWORD size,
                            uint64_t timestamp) {
    if (!f || !data || size == 0) return false;
    unsigned char h[12] = {};
    put_le32(h, (uint32_t)size);
    put_le64(h + 4, timestamp);
    return fwrite(h, 1, sizeof(h), f) == sizeof(h) &&
           fwrite(data, 1, size, f) == size;
}

static HRESULT create_video_encoder_mft(const GUID &out_subtype,
                                        IMFTransform **out,
                                        bool *is_async) {
    if (!out || !is_async) return E_POINTER;
    *out = nullptr;
    *is_async = false;

    MFT_REGISTER_TYPE_INFO type{};
    type.guidMajorType = MFMediaType_Video;
    type.guidSubtype = out_subtype;

    IMFActivate **activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_ALL,
                           nullptr, &type, &activates, &count);
    if (SUCCEEDED(hr) && count == 0) hr = MF_E_TOPO_CODEC_NOT_FOUND;
    if (SUCCEEDED(hr)) {
        hr = MF_E_TOPO_CODEC_NOT_FOUND;
        for (UINT32 i = 0; i < count; ++i) {
            ComPtr<IMFTransform> candidate;
            HRESULT activate_hr = activates[i]->ActivateObject(
                IID_PPV_ARGS(&candidate));
            if (FAILED(activate_hr) || !candidate) {
                hr = activate_hr;
                continue;
            }

            ComPtr<IMFAttributes> attributes;
            UINT32 async_flag = FALSE;
            if (SUCCEEDED(candidate->GetAttributes(&attributes)) && attributes)
                attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async_flag);
            if (async_flag) {
                HRESULT unlock_hr = attributes->SetUINT32(
                    MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                if (FAILED(unlock_hr)) {
                    hr = unlock_hr;
                    activates[i]->ShutdownObject();
                    continue;
                }
            }

            hr = candidate.CopyTo(out);
            *is_async = async_flag != FALSE;
            break;
        }
    }
    if (activates) {
        for (UINT32 i = 0; i < count; ++i) {
            if (activates[i]) activates[i]->Release();
        }
        CoTaskMemFree(activates);
    }
    return hr;
}

static HRESULT select_video_output_type(IMFTransform *encoder,
                                        IMFMediaType *preferred,
                                        const GUID &subtype,
                                        UINT32 width,
                                        UINT32 height,
                                        UINT32 fps_num,
                                        UINT32 fps_den) {
    if (!encoder || !preferred) return E_POINTER;
    HRESULT hr = encoder->SetOutputType(0, preferred, 0);
    if (SUCCEEDED(hr)) return hr;

    for (DWORD i = 0; ; ++i) {
        ComPtr<IMFMediaType> candidate;
        HRESULT type_hr = encoder->GetOutputAvailableType(0, i, &candidate);
        if (type_hr == MF_E_NO_MORE_TYPES) break;
        if (FAILED(type_hr) || !candidate) continue;

        GUID cand_subtype = GUID_NULL;
        candidate->GetGUID(MF_MT_SUBTYPE, &cand_subtype);
        if (!IsEqualGUID(cand_subtype, subtype)) continue;

        candidate->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(candidate.Get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(candidate.Get(), MF_MT_FRAME_RATE, fps_num, fps_den);
        MFSetAttributeRatio(candidate.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        type_hr = encoder->SetOutputType(0, candidate.Get(), 0);
        if (SUCCEEDED(type_hr)) return type_hr;
    }

    return hr;
}

static HRESULT select_video_input_type(IMFTransform *encoder,
                                       IMFMediaType *preferred,
                                       UINT32 width,
                                       UINT32 height,
                                       UINT32 fps_num,
                                       UINT32 fps_den,
                                       bool *use_nv12) {
    if (!encoder || !preferred || !use_nv12) return E_POINTER;
    HRESULT hr = encoder->SetInputType(0, preferred, 0);
    if (SUCCEEDED(hr)) {
        *use_nv12 = true;
        return hr;
    }

    for (DWORD i = 0; ; ++i) {
        ComPtr<IMFMediaType> candidate;
        HRESULT type_hr = encoder->GetInputAvailableType(0, i, &candidate);
        if (type_hr == MF_E_NO_MORE_TYPES) break;
        if (FAILED(type_hr) || !candidate) continue;

        GUID subtype = GUID_NULL;
        candidate->GetGUID(MF_MT_SUBTYPE, &subtype);
        bool nv12 = IsEqualGUID(subtype, MFVideoFormat_NV12);
        bool i420 = IsEqualGUID(subtype, MFVideoFormat_I420);
        if (!nv12 && !i420) continue;

        candidate->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(candidate.Get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(candidate.Get(), MF_MT_FRAME_RATE, fps_num, fps_den);
        MFSetAttributeRatio(candidate.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        candidate->SetUINT32(MF_MT_DEFAULT_STRIDE, width);
        type_hr = encoder->SetInputType(0, candidate.Get(), 0);
        if (SUCCEEDED(type_hr)) {
            *use_nv12 = nv12;
            return type_hr;
        }
    }

    return hr;
}

avb_result AvbEncoderMediaFoundation::open_ivf_video(
    const char *path, const avb_encode_options &options) {
    if (!options.video.enable || options.audio.enable) {
        m_last_error = "Media Foundation IVF output supports video only.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    avb_video_codec codec = options.video.codec;
    if (!mf_ivf_codec(codec)) {
        m_last_error = "Media Foundation IVF output supports VP8, VP9, or AV1.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    if (m_impl->input_format != AVB_PIXEL_FORMAT_I420 &&
        m_impl->input_format != AVB_PIXEL_FORMAT_NV12) {
        m_last_error = "Media Foundation IVF output currently requires I420 or NV12 input.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }

    GUID out_subtype = mf_video_subtype_from_codec(codec, 0);
    HRESULT hr = create_video_encoder_mft(
        out_subtype, &m_impl->video_encoder, &m_impl->video_encoder_async);
    if (FAILED(hr) || !m_impl->video_encoder) {
        char buf[160];
        snprintf(buf, sizeof(buf), "Create %s encoder MFT failed: 0x%08lx",
                 mf_codec_name(codec), hr);
        m_last_error = buf;
        return AVB_ERROR_OPEN_FAILED;
    }
    if (m_impl->video_encoder_async) {
        hr = m_impl->video_encoder.As(&m_impl->video_event_generator);
        if (FAILED(hr) || !m_impl->video_event_generator) {
            m_last_error = "Async video encoder does not expose IMFMediaEventGenerator.";
            return AVB_ERROR_OPEN_FAILED;
        }
    }

    ComPtr<IMFMediaType> out_type;
    MFCreateMediaType(&out_type);
    out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out_type->SetGUID(MF_MT_SUBTYPE, out_subtype);
    out_type->SetUINT32(MF_MT_AVG_BITRATE,
                        (UINT32)(options.video.bitrate > 0
                            ? options.video.bitrate : 2000000));
    out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (codec == AVB_VIDEO_CODEC_VP9)
        out_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncVP9VProfile_420_8);
    if (codec == AVB_VIDEO_CODEC_AV1)
        out_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncAV1VProfile_Main_420_8);
    MFSetAttributeSize(out_type.Get(), MF_MT_FRAME_SIZE,
                       m_impl->width, m_impl->height);
    MFSetAttributeRatio(out_type.Get(), MF_MT_FRAME_RATE,
                        m_impl->fps_num, m_impl->fps_den);
    MFSetAttributeRatio(out_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    ComPtr<IMFMediaType> in_type;
    MFCreateMediaType(&in_type);
    in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(in_type.Get(), MF_MT_FRAME_SIZE,
                       m_impl->width, m_impl->height);
    MFSetAttributeRatio(in_type.Get(), MF_MT_FRAME_RATE,
                        m_impl->fps_num, m_impl->fps_den);
    MFSetAttributeRatio(in_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    in_type->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)m_impl->width);

    bool input_type_set = false;
    hr = select_video_output_type(
        m_impl->video_encoder.Get(), out_type.Get(), out_subtype,
        (UINT32)m_impl->width, (UINT32)m_impl->height,
        m_impl->fps_num, m_impl->fps_den);
    if (FAILED(hr)) {
        bool ignored_nv12 = true;
        HRESULT input_first = select_video_input_type(
            m_impl->video_encoder.Get(), in_type.Get(),
            (UINT32)m_impl->width, (UINT32)m_impl->height,
            m_impl->fps_num, m_impl->fps_den, &ignored_nv12);
        if (SUCCEEDED(input_first)) {
            hr = select_video_output_type(
                m_impl->video_encoder.Get(), out_type.Get(), out_subtype,
                (UINT32)m_impl->width, (UINT32)m_impl->height,
                m_impl->fps_num, m_impl->fps_den);
            if (SUCCEEDED(hr)) {
                m_impl->ivf_mft_input_nv12 = ignored_nv12;
                input_type_set = true;
            }
        }
        if (FAILED(hr)) {
            char buf[160];
            snprintf(buf, sizeof(buf), "SetOutputType (%s encoder) failed: 0x%08lx",
                     mf_codec_name(codec), hr);
            m_last_error = buf;
            return AVB_ERROR_OPEN_FAILED;
        }
    }

    if (!input_type_set) {
        hr = select_video_input_type(
            m_impl->video_encoder.Get(), in_type.Get(),
            (UINT32)m_impl->width, (UINT32)m_impl->height,
            m_impl->fps_num, m_impl->fps_den, &m_impl->ivf_mft_input_nv12);
        if (FAILED(hr)) {
            char buf[160];
            snprintf(buf, sizeof(buf), "SetInputType (%s encoder) failed: 0x%08lx",
                     mf_codec_name(codec), hr);
            m_last_error = buf;
            return AVB_ERROR_OPEN_FAILED;
        }
    }

    MFT_OUTPUT_STREAM_INFO osi{};
    if (SUCCEEDED(m_impl->video_encoder->GetOutputStreamInfo(0, &osi))) {
        m_impl->video_encoder_out_size = osi.cbSize;
        m_impl->video_encoder_out_flags = osi.dwFlags;
    }
    if (m_impl->video_encoder_out_size == 0) {
        m_impl->video_encoder_out_size =
            (DWORD)std::max(65536, m_impl->width * m_impl->height * 2);
    }

    m_impl->ivf_file = fopen(path, "wb");
    if (!m_impl->ivf_file) {
        m_last_error = "Opening IVF output file failed.";
        return AVB_ERROR_OPEN_FAILED;
    }
    if (!write_ivf_header(m_impl->ivf_file, codec, m_impl->width, m_impl->height,
                          m_impl->fps_num, m_impl->fps_den, 0)) {
        m_last_error = "Writing IVF header failed.";
        return AVB_ERROR_OPEN_FAILED;
    }

    m_impl->video_encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_impl->video_encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    m_impl->ivf_video = true;
    m_impl->ivf_codec = codec;
    m_impl->has_video = true;
    m_impl->began = true;
    return AVB_OK;
}

avb_result AvbEncoderMediaFoundation::open(const char *path, const avb_encode_options &options) {
    if (!m_impl->mf_initialized) return AVB_ERROR_BACKEND_NOT_AVAILABLE;
    m_impl->reset_streams();
    m_last_error.clear();

    if (!options.video.enable && !options.audio.enable) {
        m_last_error = "Encoder requires at least one of video/audio enabled.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    if (options.video.enable) {
        if (options.video.input_memory != AVB_VIDEO_MEMORY_CPU &&
            options.video.input_memory != AVB_VIDEO_MEMORY_NATIVE) {
            m_last_error = "Media Foundation supports CPU or D3D11 native video input.";
            return AVB_ERROR_OPEN_FAILED;
        }
        if (options.video.input_memory == AVB_VIDEO_MEMORY_NATIVE &&
            options.video.input_format != AVB_PIXEL_FORMAT_UNKNOWN &&
            options.video.input_format != AVB_PIXEL_FORMAT_NV12) {
            m_last_error = "Media Foundation D3D11 native input requires NV12 textures.";
            return AVB_ERROR_INVALID_ARGUMENT;
        }
    }
    Container container = container_from_path(path, Container::unknown);
    const bool direct_ivf = container == Container::ivf;
    const bool native_sink =
        options.video.enable &&
        options.video.input_memory == AVB_VIDEO_MEMORY_NATIVE &&
        !direct_ivf;
    if (native_sink && !options.video.hardware_context) {
        m_last_error =
            "Media Foundation native MP4/MOV input requires hardware_context "
            "to point to the texture's ID3D11Device.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    if (direct_ivf && options.audio.enable) {
        m_last_error = "Media Foundation IVF output does not support audio.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }

    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (wlen <= 0) {
        m_last_error = "Invalid path encoding.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    std::wstring wpath(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath.data(), wlen);

    HRESULT hr = S_OK;
    if (!direct_ivf) {
        // Non-realtime: let WriteSample run as fast as it can rather than
        // pacing to the stream's frame rate.
        ComPtr<IMFAttributes> attrs;
        MFCreateAttributes(&attrs, native_sink ? 3 : 1);
        attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        if (native_sink) {
            hr = create_d3d11_device_manager(
                static_cast<ID3D11Device *>(options.video.hardware_context),
                &m_impl->d3d_device, &m_impl->d3d_device_manager);
            if (FAILED(hr)) {
                char buf[160];
                snprintf(buf, sizeof(buf),
                         "Creating Sink Writer D3D11 device manager failed: 0x%08lx",
                         hr);
                m_last_error = buf;
                return AVB_ERROR_OPEN_FAILED;
            }
            hr = attrs->SetUnknown(
                MF_SINK_WRITER_D3D_MANAGER,
                m_impl->d3d_device_manager.Get());
            if (FAILED(hr)) {
                char buf[160];
                snprintf(buf, sizeof(buf),
                         "Configuring Sink Writer D3D11 input failed: 0x%08lx",
                         hr);
                m_last_error = buf;
                return AVB_ERROR_OPEN_FAILED;
            }
            attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        }

        hr = MFCreateSinkWriterFromURL(
            wpath.c_str(), nullptr, attrs.Get(), &m_impl->writer);
        if (FAILED(hr)) {
            char buf[128];
            snprintf(buf, sizeof(buf), "MFCreateSinkWriterFromURL failed: 0x%08lx", hr);
            m_last_error = buf;
            return AVB_ERROR_OPEN_FAILED;
        }
    }

    // --- Video ---
    if (options.video.enable) {
        if (options.video.width <= 0 || options.video.height <= 0) {
            m_last_error = "Video width/height must be positive.";
            return AVB_ERROR_INVALID_ARGUMENT;
        }
        m_impl->width      = options.video.width;
        m_impl->height     = options.video.height;
        m_impl->input_memory = options.video.input_memory;
        m_impl->frame_rate = options.video.frame_rate > 0 ? options.video.frame_rate : 30.0;
        MFAverageTimePerFrameToFrameRate(sec_to_hns(1.0 / m_impl->frame_rate),
                                         &m_impl->fps_num, &m_impl->fps_den);
        if (m_impl->fps_den == 0) { m_impl->fps_num = 30; m_impl->fps_den = 1; }

        switch (options.video.input_format) {
            case AVB_PIXEL_FORMAT_NV12:
                m_impl->input_format = AVB_PIXEL_FORMAT_NV12;
                m_impl->video_is_nv12 = true;
                break;
            case AVB_PIXEL_FORMAT_UNKNOWN:
                if (options.video.input_memory == AVB_VIDEO_MEMORY_NATIVE) {
                    m_impl->input_format = AVB_PIXEL_FORMAT_NV12;
                    m_impl->video_is_nv12 = true;
                } else {
                    m_impl->input_format = AVB_PIXEL_FORMAT_BGRA8;
                }
                break;
            case AVB_PIXEL_FORMAT_I420:
                m_impl->input_format = AVB_PIXEL_FORMAT_I420;
                m_impl->video_is_i420 = true;
                break;
            case AVB_PIXEL_FORMAT_RGBA8:
                m_impl->input_format = AVB_PIXEL_FORMAT_RGBA8;
                m_impl->swizzle_rgba = true;
                break;
            case AVB_PIXEL_FORMAT_BGRA8:
            default:
                m_impl->input_format = AVB_PIXEL_FORMAT_BGRA8;
                break;
        }

        if (direct_ivf) {
            return open_ivf_video(path, options);
        }

        avb_video_encode_info custom_info{};
        custom_info.width = m_impl->width;
        custom_info.height = m_impl->height;
        custom_info.frame_rate = m_impl->frame_rate;
        custom_info.input_format = m_impl->input_format;
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

            GUID out_subtype = mf_video_subtype_from_codec(
                stream.codec != AVB_VIDEO_CODEC_AUTO ? stream.codec : options.video.codec,
                stream.codec_tag);
            if (IsEqualGUID(out_subtype, GUID_NULL)) {
                if (plugin->close && ctx) plugin->close(ctx);
                m_last_error = "Custom video encoder did not provide a muxable codec or codec_tag.";
                return AVB_ERROR_INVALID_ARGUMENT;
            }

            ComPtr<IMFMediaType> out_type;
            MFCreateMediaType(&out_type);
            out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            out_type->SetGUID(MF_MT_SUBTYPE, out_subtype);
            out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            if (options.video.bitrate > 0)
                out_type->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)options.video.bitrate);
            MFSetAttributeSize(out_type.Get(), MF_MT_FRAME_SIZE, m_impl->width, m_impl->height);
            MFSetAttributeRatio(out_type.Get(), MF_MT_FRAME_RATE, m_impl->fps_num, m_impl->fps_den);
            MFSetAttributeRatio(out_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            if (stream.extradata && stream.extradata_size > 0) {
                out_type->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER,
                                  stream.extradata, (UINT32)stream.extradata_size);
            }

            hr = m_impl->writer->AddStream(out_type.Get(), &m_impl->video_stream);
            if (FAILED(hr)) {
                if (plugin->close && ctx) plugin->close(ctx);
                char buf[192];
                snprintf(buf, sizeof(buf),
                         "AddStream (custom video/%s) failed: 0x%08lx",
                         stream.codec_name ? stream.codec_name : mf_codec_name(stream.codec), hr);
                m_last_error = buf;
                return AVB_ERROR_OPEN_FAILED;
            }

            hr = m_impl->writer->SetInputMediaType(m_impl->video_stream, out_type.Get(), nullptr);
            if (FAILED(hr)) {
                if (plugin->close && ctx) plugin->close(ctx);
                char buf[160];
                snprintf(buf, sizeof(buf),
                         "SetInputMediaType (custom video) failed: 0x%08lx", hr);
                m_last_error = buf;
                return AVB_ERROR_OPEN_FAILED;
            }

            m_impl->custom_video_encoder = plugin;
            m_impl->custom_video_ctx = ctx;
            m_impl->custom_video_stream = stream;
            m_impl->custom_video = true;
            m_impl->has_video = true;
        } else {

        // Choose the encoder output (compressed) subtype from the requested
        // codec. VP8/VP9/AV1 media type GUIDs and encoder MFTs can exist on
        // some Windows systems, but the built-in Sink Writer path used here
        // does not provide WebM/Matroska file sinks, so only MP4-family video
        // encoders are exposed.
        GUID     out_subtype;
        UINT32   profile = 0;       // MF_MT_MPEG2_PROFILE value, 0 = leave unset
        const char *vname;
        switch (options.video.codec) {
            case AVB_VIDEO_CODEC_AUTO:
            case AVB_VIDEO_CODEC_H264:
                out_subtype = MFVideoFormat_H264; profile = eAVEncH264VProfile_Main; vname = "H264"; break;
            case AVB_VIDEO_CODEC_HEVC:
                out_subtype = MFVideoFormat_HEVC; profile = eAVEncH265VProfile_Main_420_8; vname = "HEVC"; break;
            case AVB_VIDEO_CODEC_VP8:
                m_last_error = "VP8 encoding is not supported by the Media Foundation "
                               "file-sink backend (use the FFmpeg or GStreamer backend).";
                return AVB_ERROR_INVALID_ARGUMENT;
            case AVB_VIDEO_CODEC_VP9:
                m_last_error = "VP9 encoding is not supported by the Media Foundation "
                               "file-sink backend (use the FFmpeg or GStreamer backend).";
                return AVB_ERROR_INVALID_ARGUMENT;
            case AVB_VIDEO_CODEC_AV1:
                m_last_error = "AV1 encoding is not supported by the Media Foundation "
                               "file-sink backend (use the FFmpeg or GStreamer backend).";
                return AVB_ERROR_INVALID_ARGUMENT;
            default:
                m_last_error = "Invalid video codec (use AUTO/H264/HEVC).";
                return AVB_ERROR_INVALID_ARGUMENT;
        }

        // Choose the encoder input subtype from the caller's frame format. NV12
        // and I420 are planar and fed directly; BGRA/RGBA are fed as RGB32. The
        // Sink Writer inserts the color converter to whatever the encoder wants.
        // RGBA is swizzled to BGRA on copy.
        GUID input_subtype;
        if (m_impl->input_format == AVB_PIXEL_FORMAT_NV12) {
            m_impl->input_format  = AVB_PIXEL_FORMAT_NV12;
            m_impl->video_is_nv12 = true;
            input_subtype         = MFVideoFormat_NV12;
        } else if (m_impl->input_format == AVB_PIXEL_FORMAT_I420) {
            m_impl->input_format  = AVB_PIXEL_FORMAT_I420;
            m_impl->video_is_i420 = true;
            input_subtype         = MFVideoFormat_I420;
        } else if (m_impl->input_format == AVB_PIXEL_FORMAT_RGBA8) {
            m_impl->input_format = AVB_PIXEL_FORMAT_RGBA8;
            m_impl->swizzle_rgba = true;
            input_subtype        = MFVideoFormat_RGB32;
        } else {
            m_impl->input_format = AVB_PIXEL_FORMAT_BGRA8;
            input_subtype        = MFVideoFormat_RGB32;
        }

        int bitrate = options.video.bitrate > 0 ? options.video.bitrate : 4000000;

        // Encoded (output) type the stream is muxed as.
        ComPtr<IMFMediaType> out_type;
        MFCreateMediaType(&out_type);
        out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out_type->SetGUID(MF_MT_SUBTYPE, out_subtype);
        out_type->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)bitrate);
        out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (profile) out_type->SetUINT32(MF_MT_MPEG2_PROFILE, profile);
        MFSetAttributeSize(out_type.Get(), MF_MT_FRAME_SIZE, m_impl->width, m_impl->height);
        MFSetAttributeRatio(out_type.Get(), MF_MT_FRAME_RATE, m_impl->fps_num, m_impl->fps_den);
        MFSetAttributeRatio(out_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        hr = m_impl->writer->AddStream(out_type.Get(), &m_impl->video_stream);
        if (FAILED(hr)) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "AddStream (video/%s) failed: 0x%08lx "
                     "(the %s encoder MFT may not be installed)", vname, hr, vname);
            m_last_error = buf;
            return AVB_ERROR_OPEN_FAILED;
        }

        // Uncompressed (input) type the caller feeds.
        ComPtr<IMFMediaType> in_type;
        MFCreateMediaType(&in_type);
        in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in_type->SetGUID(MF_MT_SUBTYPE, input_subtype);
        in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        in_type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
        in_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        in_type->SetUINT32(
            MF_MT_SAMPLE_SIZE,
            (UINT32)((size_t)m_impl->width * m_impl->height * 3 / 2));
        MFSetAttributeSize(in_type.Get(), MF_MT_FRAME_SIZE, m_impl->width, m_impl->height);
        MFSetAttributeRatio(in_type.Get(), MF_MT_FRAME_RATE, m_impl->fps_num, m_impl->fps_den);
        MFSetAttributeRatio(in_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        // Positive stride marks top-down rows (our buffers); RGB32 would default
        // to bottom-up otherwise.
        in_type->SetUINT32(MF_MT_DEFAULT_STRIDE,
                           (UINT32)((m_impl->video_is_nv12 || m_impl->video_is_i420)
                                        ? m_impl->width : m_impl->width * 4));

        hr = m_impl->writer->SetInputMediaType(m_impl->video_stream, in_type.Get(), nullptr);
        if (FAILED(hr)) {
            char buf[128];
            snprintf(buf, sizeof(buf), "SetInputMediaType (video) failed: 0x%08lx", hr);
            m_last_error = buf;
            return AVB_ERROR_OPEN_FAILED;
        }
        m_impl->has_video = true;
        }
    }

    // --- Audio ---
    if (options.audio.enable) {
        if (options.audio.sample_rate <= 0 || options.audio.channels <= 0) {
            m_last_error = "Audio sample_rate/channels must be positive.";
            return AVB_ERROR_INVALID_ARGUMENT;
        }
        m_impl->sample_rate = options.audio.sample_rate;
        m_impl->channels    = options.audio.channels;
        avb_audio_codec audio_codec = options.audio.codec;
        if (audio_codec == AVB_AUDIO_CODEC_AUTO) {
            audio_codec = container == Container::wav ? AVB_AUDIO_CODEC_PCM_S16 :
                          container == Container::mp3 ? AVB_AUDIO_CODEC_MP3 :
                          container == Container::flac ? AVB_AUDIO_CODEC_FLAC :
                          AVB_AUDIO_CODEC_AAC;
        }
        m_impl->audio_codec = audio_codec;

        switch (audio_codec) {
            case AVB_AUDIO_CODEC_AUTO:
            case AVB_AUDIO_CODEC_AAC:
            case AVB_AUDIO_CODEC_MP3:
            case AVB_AUDIO_CODEC_FLAC:
            case AVB_AUDIO_CODEC_PCM_S16:
            case AVB_AUDIO_CODEC_PCM_F32:
                break;
            case AVB_AUDIO_CODEC_OPUS:
            case AVB_AUDIO_CODEC_VORBIS:
                m_last_error = "Requested audio codec is not supported by the Media Foundation "
                               "backend yet (use the FFmpeg or GStreamer backend).";
                return AVB_ERROR_INVALID_ARGUMENT;
            default:
                m_last_error = "Invalid audio codec.";
                return AVB_ERROR_INVALID_ARGUMENT;
        }

        const bool float_pcm = audio_codec == AVB_AUDIO_CODEC_PCM_F32;
        const UINT32 bits_per_sample = float_pcm ? 32 : 16;
        const UINT32 block_align =
            (UINT32)m_impl->channels * (bits_per_sample / 8);
        const UINT32 pcm_bytes_per_sec = (UINT32)m_impl->sample_rate * block_align;

        // PCM output is also the input type; no encoder MFT is needed when the
        // selected sink is WAV.
        ComPtr<IMFMediaType> out_type;
        MFCreateMediaType(&out_type);
        if (audio_codec == AVB_AUDIO_CODEC_PCM_S16 ||
            audio_codec == AVB_AUDIO_CODEC_PCM_F32) {
            out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            out_type->SetGUID(MF_MT_SUBTYPE,
                              float_pcm ? MFAudioFormat_Float : MFAudioFormat_PCM);
            out_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, bits_per_sample);
            out_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, block_align);
            out_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, pcm_bytes_per_sec);
            out_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        } else if (audio_codec == AVB_AUDIO_CODEC_MP3) {
            int bitrate = mf_mp3_bitrate(options.audio.bitrate);
            hr = init_mp3_media_type(out_type.Get(), m_impl->sample_rate,
                                     m_impl->channels, bitrate);
            if (FAILED(hr)) {
                char buf[160];
                snprintf(buf, sizeof(buf),
                         "MFInitMediaTypeFromWaveFormatEx (MP3) failed: 0x%08lx", hr);
                m_last_error = buf;
                return AVB_ERROR_OPEN_FAILED;
            }
        } else if (audio_codec == AVB_AUDIO_CODEC_FLAC) {
            out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            out_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_FLAC);
            out_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            out_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, block_align);
            out_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                                pcm_bytes_per_sec);
            out_type->SetUINT32(MF_MT_AUDIO_FLAC_MAX_BLOCK_SIZE, 4096);
        } else {
            out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            out_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            out_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            out_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                                aac_bytes_per_sec(options.audio.bitrate));
        }
        if (audio_codec != AVB_AUDIO_CODEC_MP3) {
            out_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
                                (UINT32)m_impl->sample_rate);
            out_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)m_impl->channels);
        }

        hr = m_impl->writer->AddStream(out_type.Get(), &m_impl->audio_stream);
        if (FAILED(hr)) {
            char buf[128];
            snprintf(buf, sizeof(buf), "AddStream (audio/%s) failed: 0x%08lx",
                     avb_audio_codec_name(audio_codec), hr);
            m_last_error = buf;
            return AVB_ERROR_OPEN_FAILED;
        }

        // Input type is float PCM for PCM_F32, otherwise interleaved S16 PCM.
        ComPtr<IMFMediaType> in_type;
        MFCreateMediaType(&in_type);
        in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        in_type->SetGUID(MF_MT_SUBTYPE,
                         float_pcm ? MFAudioFormat_Float : MFAudioFormat_PCM);
        in_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, bits_per_sample);
        in_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)m_impl->sample_rate);
        in_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)m_impl->channels);
        in_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, block_align);
        in_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, pcm_bytes_per_sec);
        in_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

        if (audio_codec == AVB_AUDIO_CODEC_MP3) {
            hr = CoCreateInstance(CLSID_MP3ACMCodecWrapper, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_impl->audio_encoder));
            if (FAILED(hr) || !m_impl->audio_encoder) {
                char buf[160];
                snprintf(buf, sizeof(buf),
                         "CoCreateInstance (MP3 ACM wrapper) failed: 0x%08lx", hr);
                m_last_error = buf;
                return AVB_ERROR_OPEN_FAILED;
            }

            ComPtr<IMFMediaType> selected_out_type;
            hr = select_mp3_output_type(
                m_impl->audio_encoder.Get(), out_type.Get(),
                (UINT32)m_impl->sample_rate, (UINT32)m_impl->channels,
                &selected_out_type);
            if (FAILED(hr)) {
                char buf[160];
                snprintf(buf, sizeof(buf),
                         "SetOutputType (MP3 encoder) failed: 0x%08lx", hr);
                m_last_error = buf;
                return AVB_ERROR_OPEN_FAILED;
            }
            out_type = selected_out_type;

            hr = m_impl->audio_encoder->SetInputType(0, in_type.Get(), 0);
            if (FAILED(hr)) {
                char buf[160];
                snprintf(buf, sizeof(buf),
                         "SetInputType (MP3 encoder) failed: 0x%08lx", hr);
                m_last_error = buf;
                return AVB_ERROR_OPEN_FAILED;
            }

            MFT_OUTPUT_STREAM_INFO osi{};
            if (SUCCEEDED(m_impl->audio_encoder->GetOutputStreamInfo(0, &osi)) &&
                osi.cbSize > 0) {
                m_impl->audio_encoder_out_size = osi.cbSize;
            } else {
                m_impl->audio_encoder_out_size = 16384;
            }
            m_impl->audio_encoder->ProcessMessage(
                MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
            m_impl->audio_encoder->ProcessMessage(
                MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
            m_impl->audio_encoded_by_mft = true;
        } else {
            // MP3 is encoded through CLSID_MP3ACMCodecWrapper above and the
            // compressed samples are written directly to the sink stream.
            // Calling SetInputMediaType for that stream asks Sink Writer to
            // encode again and is rejected on current Windows builds.
            hr = m_impl->writer->SetInputMediaType(m_impl->audio_stream, in_type.Get(), nullptr);
            if (FAILED(hr)) {
                char buf[160];
                snprintf(buf, sizeof(buf),
                         "SetInputMediaType (audio/%s) failed: 0x%08lx",
                         avb_audio_codec_name(audio_codec), hr);
                m_last_error = buf;
                return AVB_ERROR_OPEN_FAILED;
            }
        }
        m_impl->has_audio = true;
    }

    hr = m_impl->writer->BeginWriting();
    if (FAILED(hr)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "BeginWriting failed: 0x%08lx", hr);
        m_last_error = buf;
        return AVB_ERROR_OPEN_FAILED;
    }
    m_impl->began = true;
    return AVB_OK;
}

// Wrap a freshly filled, MF-allocated buffer in a sample with timing and write
// it to the given stream. Takes ownership semantics of `buf` via ComPtr.
static HRESULT write_buffer(IMFSinkWriter *writer, DWORD stream,
                            ComPtr<IMFMediaBuffer> &buf, DWORD length,
                            LONGLONG time_hns, LONGLONG dur_hns) {
    buf->SetCurrentLength(length);
    ComPtr<IMFSample> sample;
    HRESULT hr = MFCreateSample(&sample);
    if (FAILED(hr)) return hr;
    hr = sample->AddBuffer(buf.Get());
    if (FAILED(hr)) return hr;
    sample->SetSampleTime(time_hns);
    sample->SetSampleDuration(dur_hns);
    return writer->WriteSample(stream, sample.Get());
}

avb_result AvbEncoderMediaFoundation::drain_video_mft(long long,
                                                       long long) {
    if (!m_impl || !m_impl->video_encoder || !m_impl->ivf_file)
        return AVB_ERROR_INVALID_ARGUMENT;

    for (;;) {
        avb_result result = process_video_mft_output();
        if (result == AVB_ERROR_AGAIN) return AVB_OK;
        if (result != AVB_OK) return result;
    }
}

avb_result AvbEncoderMediaFoundation::process_video_mft_output() {
    if (!m_impl || !m_impl->video_encoder || !m_impl->ivf_file)
        return AVB_ERROR_INVALID_ARGUMENT;

    ComPtr<IMFSample> out_sample;
    MFT_OUTPUT_DATA_BUFFER output{};
    output.dwStreamID = 0;

    if ((m_impl->video_encoder_out_flags &
         MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
        ComPtr<IMFMediaBuffer> out_buf;
        HRESULT bhr = MFCreateMemoryBuffer(
            m_impl->video_encoder_out_size > 0
                ? m_impl->video_encoder_out_size : 65536,
            &out_buf);
        if (FAILED(bhr)) {
            m_last_error = "MFCreateMemoryBuffer (IVF output) failed.";
            return AVB_ERROR_ENCODE_FAILED;
        }
        bhr = MFCreateSample(&out_sample);
        if (FAILED(bhr)) {
            m_last_error = "MFCreateSample (IVF output) failed.";
            return AVB_ERROR_ENCODE_FAILED;
        }
        out_sample->AddBuffer(out_buf.Get());
        output.pSample = out_sample.Get();
    }

    DWORD status = 0;
    HRESULT hr = m_impl->video_encoder->ProcessOutput(0, 1, &output, &status);
    if (output.pEvents) output.pEvents->Release();

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return AVB_ERROR_AGAIN;
    if (FAILED(hr)) {
        char b[160];
        snprintf(b, sizeof(b), "ProcessOutput (IVF video encoder) failed: 0x%08lx", hr);
        m_last_error = b;
        if (output.pSample && output.pSample != out_sample.Get())
            output.pSample->Release();
        return AVB_ERROR_ENCODE_FAILED;
    }

    IMFSample *sample = output.pSample ? output.pSample : out_sample.Get();
    ComPtr<IMFMediaBuffer> encoded;
    if (sample) sample->ConvertToContiguousBuffer(&encoded);
    if (output.pSample && output.pSample != out_sample.Get())
        output.pSample->Release();

    DWORD length = 0;
    if (encoded) encoded->GetCurrentLength(&length);
    if (encoded && length > 0) {
        BYTE *data = nullptr;
        DWORD max_len = 0, cur_len = 0;
        hr = encoded->Lock(&data, &max_len, &cur_len);
        if (FAILED(hr)) {
            m_last_error = "Lock (IVF encoded buffer) failed.";
            return AVB_ERROR_ENCODE_FAILED;
        }
        bool ok = write_ivf_frame(
            m_impl->ivf_file, data, cur_len, m_impl->ivf_frame_count);
        encoded->Unlock();
        if (!ok) {
            m_last_error = "Writing IVF frame failed.";
            return AVB_ERROR_ENCODE_FAILED;
        }
        m_impl->ivf_frame_count++;
    }
    return AVB_OK;
}

static HRESULT get_mft_event_with_timeout(IMFMediaEventGenerator *events,
                                          DWORD timeout_ms,
                                          IMFMediaEvent **out) {
    if (!events || !out) return E_POINTER;
    *out = nullptr;
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        HRESULT hr = events->GetEvent(MF_EVENT_FLAG_NO_WAIT, out);
        if (hr != MF_E_NO_EVENTS_AVAILABLE) return hr;
        if (GetTickCount64() >= deadline) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        Sleep(1);
    }
}

avb_result AvbEncoderMediaFoundation::wait_async_video_input() {
    if (!m_impl || !m_impl->video_event_generator)
        return AVB_ERROR_INVALID_ARGUMENT;

    for (;;) {
        ComPtr<IMFMediaEvent> event;
        HRESULT hr = get_mft_event_with_timeout(
            m_impl->video_event_generator.Get(), 10000, &event);
        if (FAILED(hr)) {
            char b[160];
            snprintf(b, sizeof(b), "Waiting for async video encoder input failed: 0x%08lx", hr);
            m_last_error = b;
            return AVB_ERROR_ENCODE_FAILED;
        }
        HRESULT event_status = S_OK;
        event->GetStatus(&event_status);
        if (FAILED(event_status)) {
            char b[160];
            snprintf(b, sizeof(b), "Async video encoder event failed: 0x%08lx", event_status);
            m_last_error = b;
            return AVB_ERROR_ENCODE_FAILED;
        }
        MediaEventType type = MEUnknown;
        event->GetType(&type);
        if (type == METransformNeedInput) return AVB_OK;
        if (type == METransformHaveOutput) {
            avb_result result = process_video_mft_output();
            if (result != AVB_OK && result != AVB_ERROR_AGAIN) return result;
        }
    }
}

avb_result AvbEncoderMediaFoundation::drain_async_video_mft() {
    if (!m_impl || !m_impl->video_event_generator)
        return AVB_ERROR_INVALID_ARGUMENT;

    for (;;) {
        ComPtr<IMFMediaEvent> event;
        HRESULT hr = get_mft_event_with_timeout(
            m_impl->video_event_generator.Get(), 10000, &event);
        if (FAILED(hr)) {
            char b[160];
            snprintf(b, sizeof(b), "Draining async video encoder failed: 0x%08lx", hr);
            m_last_error = b;
            return AVB_ERROR_ENCODE_FAILED;
        }
        HRESULT event_status = S_OK;
        event->GetStatus(&event_status);
        if (FAILED(event_status)) {
            char b[160];
            snprintf(b, sizeof(b), "Async video encoder drain event failed: 0x%08lx", event_status);
            m_last_error = b;
            return AVB_ERROR_ENCODE_FAILED;
        }
        MediaEventType type = MEUnknown;
        event->GetType(&type);
        if (type == METransformDrainComplete) return AVB_OK;
        if (type == METransformHaveOutput) {
            avb_result result = process_video_mft_output();
            if (result != AVB_OK && result != AVB_ERROR_AGAIN) return result;
        }
    }
}

avb_result AvbEncoderMediaFoundation::drain_audio_mft(long long time_hns,
                                                       long long dur_hns) {
    if (!m_impl || !m_impl->audio_encoder) return AVB_ERROR_INVALID_ARGUMENT;

    for (;;) {
        ComPtr<IMFMediaBuffer> out_buf;
        HRESULT hr = MFCreateMemoryBuffer(
            m_impl->audio_encoder_out_size > 0 ? m_impl->audio_encoder_out_size : 16384,
            &out_buf);
        if (FAILED(hr)) {
            m_last_error = "MFCreateMemoryBuffer (MP3 output) failed.";
            return AVB_ERROR_ENCODE_FAILED;
        }

        ComPtr<IMFSample> out_sample;
        hr = MFCreateSample(&out_sample);
        if (FAILED(hr)) {
            m_last_error = "MFCreateSample (MP3 output) failed.";
            return AVB_ERROR_ENCODE_FAILED;
        }
        out_sample->AddBuffer(out_buf.Get());

        MFT_OUTPUT_DATA_BUFFER output{};
        output.dwStreamID = 0;
        output.pSample = out_sample.Get();
        DWORD status = 0;
        hr = m_impl->audio_encoder->ProcessOutput(0, 1, &output, &status);
        if (output.pEvents) output.pEvents->Release();

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return AVB_OK;
        if (FAILED(hr)) {
            char b[160];
            snprintf(b, sizeof(b), "ProcessOutput (MP3 encoder) failed: 0x%08lx", hr);
            m_last_error = b;
            return AVB_ERROR_ENCODE_FAILED;
        }

        ComPtr<IMFMediaBuffer> encoded;
        out_sample->ConvertToContiguousBuffer(&encoded);
        DWORD length = 0;
        if (encoded) encoded->GetCurrentLength(&length);
        if (encoded && length > 0) {
            hr = write_buffer(m_impl->writer.Get(), m_impl->audio_stream, encoded,
                              length, (LONGLONG)time_hns, (LONGLONG)dur_hns);
            if (FAILED(hr)) {
                char b[160];
                snprintf(b, sizeof(b), "WriteSample (MP3) failed: 0x%08lx", hr);
                m_last_error = b;
                return AVB_ERROR_ENCODE_FAILED;
            }
        }
    }
}

avb_result AvbEncoderMediaFoundation::write_video(const avb_video_frame &frame, double pts_sec) {
    if (!m_impl->has_video) return AVB_ERROR_INVALID_ARGUMENT;
    if (m_impl->custom_video) {
        avb_encoded_packet packet{};
        avb_result res = m_impl->custom_video_encoder->encode_frame(
            m_impl->custom_video_ctx, &frame, pts_sec, &packet);
        if (res != AVB_OK) return res;
        if (!packet.data || packet.size <= 0) {
            if (m_impl->custom_video_encoder->release_packet)
                m_impl->custom_video_encoder->release_packet(m_impl->custom_video_ctx, &packet);
            m_last_error = "Custom video encoder returned an empty packet.";
            return AVB_ERROR_ENCODE_FAILED;
        }

        double pts = packet.pts_sec >= 0.0 ? packet.pts_sec
                   : pts_sec >= 0.0 ? pts_sec
                   : frame.pts_sec >= 0.0 ? frame.pts_sec
                   : (double)m_impl->video_index / m_impl->frame_rate;
        double dur = packet.duration_sec > 0.0
                   ? packet.duration_sec : 1.0 / m_impl->frame_rate;

        ComPtr<IMFMediaBuffer> buf;
        HRESULT hr = MFCreateMemoryBuffer((DWORD)packet.size, &buf);
        if (FAILED(hr)) {
            if (m_impl->custom_video_encoder->release_packet)
                m_impl->custom_video_encoder->release_packet(m_impl->custom_video_ctx, &packet);
            m_last_error = "MFCreateMemoryBuffer (custom video) failed.";
            return AVB_ERROR_ENCODE_FAILED;
        }
        BYTE *data = nullptr;
        if (FAILED(buf->Lock(&data, nullptr, nullptr))) {
            if (m_impl->custom_video_encoder->release_packet)
                m_impl->custom_video_encoder->release_packet(m_impl->custom_video_ctx, &packet);
            m_last_error = "Lock (custom video buffer) failed.";
            return AVB_ERROR_ENCODE_FAILED;
        }
        memcpy(data, packet.data, packet.size);
        buf->Unlock();

        hr = write_buffer(m_impl->writer.Get(), m_impl->video_stream, buf,
                          (DWORD)packet.size, sec_to_hns(pts), sec_to_hns(dur));
        if (m_impl->custom_video_encoder->release_packet)
            m_impl->custom_video_encoder->release_packet(m_impl->custom_video_ctx, &packet);
        if (FAILED(hr)) {
            char b[144];
            snprintf(b, sizeof(b), "WriteSample (custom video) failed: 0x%08lx", hr);
            m_last_error = b;
            return AVB_ERROR_ENCODE_FAILED;
        }
        m_impl->video_index++;
        return AVB_OK;
    }
    if (frame.format != m_impl->input_format) {
        m_last_error = "Frame pixel format does not match configured input_format.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }

    const int w = m_impl->width;
    const int h = m_impl->height;

    // PTS resolution order: explicit arg, then the frame's own pts (so decoded
    // frames pass straight through), then derived from frame_rate.
    double pts = pts_sec >= 0.0       ? pts_sec
               : frame.pts_sec >= 0.0 ? frame.pts_sec
               : (double)m_impl->video_index / m_impl->frame_rate;
    LONGLONG time_hns = sec_to_hns(pts);
    LONGLONG dur_hns  = sec_to_hns(1.0 / m_impl->frame_rate);

    if (m_impl->input_memory == AVB_VIDEO_MEMORY_NATIVE) {
        if (frame.memory_type != AVB_VIDEO_MEMORY_NATIVE ||
            frame.hardware_device != AVB_HW_DEVICE_D3D11VA ||
            !frame.native_handle) {
            m_last_error = "Native video input requires an ID3D11Texture2D native_handle.";
            return AVB_ERROR_INVALID_ARGUMENT;
        }
        auto *texture = static_cast<ID3D11Texture2D *>(frame.native_handle);
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if ((int)desc.Width != m_impl->width ||
            (int)desc.Height != m_impl->height ||
            desc.Format != DXGI_FORMAT_NV12 ||
            frame.native_handle_id >= desc.ArraySize) {
            m_last_error = "D3D11 texture must match the configured NV12 dimensions and subresource.";
            return AVB_ERROR_INVALID_ARGUMENT;
        }

        ComPtr<IMFMediaBuffer> surface_buffer;
        HRESULT hr = MFCreateDXGISurfaceBuffer(
            __uuidof(ID3D11Texture2D), texture,
            (UINT)frame.native_handle_id, FALSE, &surface_buffer);
        if (FAILED(hr)) {
            char b[160];
            snprintf(b, sizeof(b), "MFCreateDXGISurfaceBuffer failed: 0x%08lx", hr);
            m_last_error = b;
            return AVB_ERROR_ENCODE_FAILED;
        }
        hr = surface_buffer->SetCurrentLength(
            (DWORD)((size_t)m_impl->width * m_impl->height * 3 / 2));
        if (FAILED(hr)) {
            char b[160];
            snprintf(b, sizeof(b),
                     "SetCurrentLength (D3D11 video buffer) failed: 0x%08lx",
                     hr);
            m_last_error = b;
            return AVB_ERROR_ENCODE_FAILED;
        }
        ComPtr<IMFSample> sample;
        hr = MFCreateSample(&sample);
        if (FAILED(hr) || FAILED(sample->AddBuffer(surface_buffer.Get()))) {
            m_last_error = "Creating D3D11 video sample failed.";
            return AVB_ERROR_ENCODE_FAILED;
        }
        sample->SetSampleTime(time_hns);
        sample->SetSampleDuration(dur_hns);

        if (m_impl->ivf_video) {
            if (m_impl->video_encoder_async) {
                avb_result ready = wait_async_video_input();
                if (ready != AVB_OK) return ready;
            }
            hr = m_impl->video_encoder->ProcessInput(0, sample.Get(), 0);
            if (FAILED(hr)) {
                char b[160];
                snprintf(b, sizeof(b),
                         "ProcessInput (native IVF video) failed: 0x%08lx", hr);
                m_last_error = b;
                return AVB_ERROR_ENCODE_FAILED;
            }
            if (!m_impl->video_encoder_async) {
                avb_result drained = drain_video_mft(time_hns, dur_hns);
                if (drained != AVB_OK) return drained;
            }
        } else {
            hr = m_impl->writer->WriteSample(m_impl->video_stream, sample.Get());
            if (FAILED(hr)) {
                char b[160];
                snprintf(b, sizeof(b),
                         "WriteSample (native video) failed: 0x%08lx", hr);
                m_last_error = b;
                return AVB_ERROR_ENCODE_FAILED;
            }
        }
        ++m_impl->video_index;
        return AVB_OK;
    }

    // Repack the caller's frame into a tightly-packed, top-down buffer matching
    // the declared input stride, then hand it to the Sink Writer.
    DWORD total;
    if (m_impl->video_is_nv12) {
        const int    y_rows = h, c_rows = h / 2, dst_stride = w;
        const size_t y_size = (size_t)dst_stride * y_rows;
        const size_t c_size = (size_t)dst_stride * c_rows;
        total = (DWORD)(y_size + c_size);
        m_impl->video_stage.resize(total);
        unsigned char *dst = m_impl->video_stage.data();
        for (int y = 0; y < y_rows; ++y)
            memcpy(dst + (size_t)y * dst_stride,
                   frame.plane_data[0] + (size_t)y * frame.plane_stride[0], dst_stride);
        for (int y = 0; y < c_rows; ++y)
            memcpy(dst + y_size + (size_t)y * dst_stride,
                   frame.plane_data[1] + (size_t)y * frame.plane_stride[1], dst_stride);
    } else if (m_impl->video_is_i420) {
        const int    cw = w / 2, ch = h / 2;
        const size_t y_size = (size_t)w * h;
        const size_t c_size = (size_t)cw * ch;
        if (m_impl->ivf_video && m_impl->ivf_mft_input_nv12) {
            // The VP encoder MFTs accept NV12 more broadly than I420. Keep the
            // public input as I420, but interleave U/V for the MFT.
            total = (DWORD)(y_size + (size_t)w * ch);
            m_impl->video_stage.resize(total);
            unsigned char *dst = m_impl->video_stage.data();
            for (int y = 0; y < h; ++y)
                memcpy(dst + (size_t)y * w,
                       frame.plane_data[0] + (size_t)y * frame.plane_stride[0], w);
            unsigned char *uv = dst + y_size;
            for (int y = 0; y < ch; ++y) {
                const unsigned char *u =
                    frame.plane_data[1] + (size_t)y * frame.plane_stride[1];
                const unsigned char *v =
                    frame.plane_data[2] + (size_t)y * frame.plane_stride[2];
                for (int x = 0; x < cw; ++x) {
                    uv[(size_t)y * w + x * 2 + 0] = u[x];
                    uv[(size_t)y * w + x * 2 + 1] = v[x];
                }
            }
        } else {
            // Three tightly-packed planes: Y (w x h), then Cb and Cr (w/2 x h/2).
            total = (DWORD)(y_size + 2 * c_size);
            m_impl->video_stage.resize(total);
            unsigned char *dst = m_impl->video_stage.data();
            for (int y = 0; y < h; ++y)
                memcpy(dst + (size_t)y * w,
                       frame.plane_data[0] + (size_t)y * frame.plane_stride[0], w);
            for (int y = 0; y < ch; ++y)
                memcpy(dst + y_size + (size_t)y * cw,
                       frame.plane_data[1] + (size_t)y * frame.plane_stride[1], cw);
            for (int y = 0; y < ch; ++y)
                memcpy(dst + y_size + c_size + (size_t)y * cw,
                       frame.plane_data[2] + (size_t)y * frame.plane_stride[2], cw);
        }
    } else {
        const int dst_stride = w * 4;
        total = (DWORD)((size_t)dst_stride * h);
        m_impl->video_stage.resize(total);
        unsigned char *dst = m_impl->video_stage.data();
        for (int y = 0; y < h; ++y) {
            const unsigned char *src = frame.plane_data[0] + (size_t)y * frame.plane_stride[0];
            unsigned char       *row = dst + (size_t)y * dst_stride;
            if (m_impl->swizzle_rgba) {
                for (int x = 0; x < w; ++x) { // RGBA -> BGRA
                    row[x * 4 + 0] = src[x * 4 + 2];
                    row[x * 4 + 1] = src[x * 4 + 1];
                    row[x * 4 + 2] = src[x * 4 + 0];
                    row[x * 4 + 3] = src[x * 4 + 3];
                }
            } else {
                memcpy(row, src, dst_stride);
            }
        }
    }

    ComPtr<IMFMediaBuffer> buf;
    HRESULT hr = MFCreateMemoryBuffer(total, &buf);
    if (FAILED(hr)) { m_last_error = "MFCreateMemoryBuffer (video) failed."; return AVB_ERROR_ENCODE_FAILED; }

    BYTE *data = nullptr;
    if (FAILED(buf->Lock(&data, nullptr, nullptr))) {
        m_last_error = "Lock (video buffer) failed."; return AVB_ERROR_ENCODE_FAILED;
    }
    memcpy(data, m_impl->video_stage.data(), total);
    buf->Unlock();

    if (m_impl->ivf_video) {
        buf->SetCurrentLength(total);
        ComPtr<IMFSample> sample;
        hr = MFCreateSample(&sample);
        if (FAILED(hr)) {
            m_last_error = "MFCreateSample (IVF input) failed.";
            return AVB_ERROR_ENCODE_FAILED;
        }
        sample->AddBuffer(buf.Get());
        sample->SetSampleTime(time_hns);
        sample->SetSampleDuration(dur_hns);

        if (m_impl->video_encoder_async) {
            avb_result ready = wait_async_video_input();
            if (ready != AVB_OK) return ready;
        }
        hr = m_impl->video_encoder->ProcessInput(0, sample.Get(), 0);
        if (!m_impl->video_encoder_async && hr == MF_E_NOTACCEPTING) {
            avb_result drained = drain_video_mft(time_hns, dur_hns);
            if (drained != AVB_OK) return drained;
            hr = m_impl->video_encoder->ProcessInput(0, sample.Get(), 0);
        }
        if (FAILED(hr)) {
            char b[160];
            snprintf(b, sizeof(b), "ProcessInput (IVF video encoder) failed: 0x%08lx", hr);
            m_last_error = b;
            return AVB_ERROR_ENCODE_FAILED;
        }

        if (!m_impl->video_encoder_async) {
            avb_result drained = drain_video_mft(time_hns, dur_hns);
            if (drained != AVB_OK) return drained;
        }
        m_impl->video_index++;
        return AVB_OK;
    }

    hr = write_buffer(m_impl->writer.Get(), m_impl->video_stream, buf, total, time_hns, dur_hns);
    if (FAILED(hr)) {
        char b[128];
        snprintf(b, sizeof(b), "WriteSample (video) failed: 0x%08lx", hr);
        m_last_error = b;
        return AVB_ERROR_ENCODE_FAILED;
    }
    m_impl->video_index++;
    return AVB_OK;
}

avb_result AvbEncoderMediaFoundation::write_audio_f32(const float *src_interleaved, int frames) {
    if (!m_impl->has_audio) return AVB_ERROR_INVALID_ARGUMENT;

    const int    nch    = m_impl->channels;
    const size_t count  = (size_t)frames * nch;
    const bool float_pcm = m_impl->audio_codec == AVB_AUDIO_CODEC_PCM_F32;
    const DWORD nbytes = (DWORD)(count *
        (float_pcm ? sizeof(float) : sizeof(int16_t)));

    const void *audio_data = src_interleaved;
    if (!float_pcm) {
        // AAC, MP3, FLAC, and PCM_S16 consume interleaved 16-bit PCM.
        m_impl->audio_stage.resize(count);
        int16_t *out = m_impl->audio_stage.data();
        for (size_t i = 0; i < count; ++i) {
            float s = src_interleaved[i];
            s = std::max(-1.0f, std::min(1.0f, s));
            out[i] = (int16_t)std::lround(s * 32767.0f);
        }
        audio_data = out;
    }

    ComPtr<IMFMediaBuffer> buf;
    HRESULT hr = MFCreateMemoryBuffer(nbytes, &buf);
    if (FAILED(hr)) { m_last_error = "MFCreateMemoryBuffer (audio) failed."; return AVB_ERROR_ENCODE_FAILED; }

    BYTE *data = nullptr;
    if (FAILED(buf->Lock(&data, nullptr, nullptr))) {
        m_last_error = "Lock (audio buffer) failed."; return AVB_ERROR_ENCODE_FAILED;
    }
    memcpy(data, audio_data, nbytes);
    buf->Unlock();

    LONGLONG time_hns = sec_to_hns((double)m_impl->audio_samples / m_impl->sample_rate);
    LONGLONG dur_hns  = sec_to_hns((double)frames / m_impl->sample_rate);

    if (m_impl->audio_encoded_by_mft) {
        buf->SetCurrentLength(nbytes);
        ComPtr<IMFSample> sample;
        hr = MFCreateSample(&sample);
        if (FAILED(hr)) {
            m_last_error = "MFCreateSample (MP3 input) failed.";
            return AVB_ERROR_ENCODE_FAILED;
        }
        sample->AddBuffer(buf.Get());
        sample->SetSampleTime(time_hns);
        sample->SetSampleDuration(dur_hns);

        hr = m_impl->audio_encoder->ProcessInput(0, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            avb_result drained = drain_audio_mft(time_hns, dur_hns);
            if (drained != AVB_OK) return drained;
            hr = m_impl->audio_encoder->ProcessInput(0, sample.Get(), 0);
        }
        if (FAILED(hr)) {
            char b[160];
            snprintf(b, sizeof(b), "ProcessInput (MP3 encoder) failed: 0x%08lx", hr);
            m_last_error = b;
            return AVB_ERROR_ENCODE_FAILED;
        }

        avb_result drained = drain_audio_mft(time_hns, dur_hns);
        if (drained != AVB_OK) return drained;
        m_impl->audio_samples += frames;
        return AVB_OK;
    }

    hr = write_buffer(m_impl->writer.Get(), m_impl->audio_stream, buf, nbytes, time_hns, dur_hns);
    if (FAILED(hr)) {
        char b[128];
        snprintf(b, sizeof(b), "WriteSample (audio) failed: 0x%08lx", hr);
        m_last_error = b;
        return AVB_ERROR_ENCODE_FAILED;
    }
    m_impl->audio_samples += frames;
    return AVB_OK;
}

avb_result AvbEncoderMediaFoundation::finish() {
    if ((!m_impl->writer && !m_impl->ivf_video) || !m_impl->began)
        return AVB_ERROR_INVALID_ARGUMENT;
    if (m_impl->finished) return AVB_OK;

    if (m_impl->ivf_video && m_impl->video_encoder) {
        m_impl->video_encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_impl->video_encoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        avb_result drained = m_impl->video_encoder_async
            ? drain_async_video_mft()
            : drain_video_mft(
                sec_to_hns((double)m_impl->video_index / m_impl->frame_rate), 0);
        if (drained != AVB_OK) return drained;

        if (m_impl->ivf_file) {
            if (fseek(m_impl->ivf_file, 0, SEEK_SET) != 0 ||
                !write_ivf_header(m_impl->ivf_file, m_impl->ivf_codec,
                                  m_impl->width, m_impl->height,
                                  m_impl->fps_num, m_impl->fps_den,
                                  m_impl->ivf_frame_count)) {
                m_last_error = "Updating IVF header failed.";
                return AVB_ERROR_ENCODE_FAILED;
            }
            fclose(m_impl->ivf_file);
            m_impl->ivf_file = nullptr;
        }
        m_impl->finished = true;
        return AVB_OK;
    }

    if (m_impl->custom_video) {
        while (m_impl->custom_video_encoder->flush) {
            avb_encoded_packet packet{};
            avb_result r = m_impl->custom_video_encoder->flush(
                m_impl->custom_video_ctx, &packet);
            if (r == AVB_ERROR_EOF || r == AVB_ERROR_AGAIN) break;
            if (r != AVB_OK) return r;
            if (packet.data && packet.size > 0) {
                double pts = packet.pts_sec >= 0.0
                    ? packet.pts_sec : (double)m_impl->video_index / m_impl->frame_rate;
                double dur = packet.duration_sec > 0.0
                    ? packet.duration_sec : 1.0 / m_impl->frame_rate;
                ComPtr<IMFMediaBuffer> buf;
                HRESULT bhr = MFCreateMemoryBuffer((DWORD)packet.size, &buf);
                if (FAILED(bhr)) return AVB_ERROR_ENCODE_FAILED;
                BYTE *data = nullptr;
                if (FAILED(buf->Lock(&data, nullptr, nullptr))) return AVB_ERROR_ENCODE_FAILED;
                memcpy(data, packet.data, packet.size);
                buf->Unlock();
                bhr = write_buffer(m_impl->writer.Get(), m_impl->video_stream, buf,
                                   (DWORD)packet.size, sec_to_hns(pts), sec_to_hns(dur));
                if (FAILED(bhr)) return AVB_ERROR_ENCODE_FAILED;
                m_impl->video_index++;
            }
            if (m_impl->custom_video_encoder->release_packet)
                m_impl->custom_video_encoder->release_packet(m_impl->custom_video_ctx, &packet);
        }
    }

    if (m_impl->audio_encoded_by_mft && m_impl->audio_encoder) {
        m_impl->audio_encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_impl->audio_encoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        avb_result drained = drain_audio_mft(
            sec_to_hns((double)m_impl->audio_samples / m_impl->sample_rate), 0);
        if (drained != AVB_OK) return drained;
    }

    HRESULT hr = m_impl->writer->Finalize();
    if (FAILED(hr)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Finalize failed: 0x%08lx", hr);
        m_last_error = buf;
        return AVB_ERROR_ENCODE_FAILED;
    }
    m_impl->finished = true;
    return AVB_OK;
}

#else // !_WIN32

AvbEncoderMediaFoundation::AvbEncoderMediaFoundation() { m_impl = nullptr; }
AvbEncoderMediaFoundation::~AvbEncoderMediaFoundation() {}
avb_result AvbEncoderMediaFoundation::open(const char *, const avb_encode_options &) {
    return AVB_ERROR_BACKEND_NOT_AVAILABLE;
}
avb_result AvbEncoderMediaFoundation::write_video(const avb_video_frame &, double) {
    return AVB_ERROR_BACKEND_NOT_AVAILABLE;
}
avb_result AvbEncoderMediaFoundation::write_audio_f32(const float *, int) {
    return AVB_ERROR_BACKEND_NOT_AVAILABLE;
}
avb_result AvbEncoderMediaFoundation::finish() { return AVB_ERROR_BACKEND_NOT_AVAILABLE; }
const char *AvbEncoderMediaFoundation::get_last_error() const {
    return m_last_error.empty() ? nullptr : m_last_error.c_str();
}

#endif // _WIN32
