#include "avb_stream_mediafoundation.hpp"
#include "avb_bitstream.hpp"
#include "avb_mediafoundation_common.hpp"
#include "avb_mediafoundation_decode_frames.hpp"
#include "avb_mediafoundation_encode_types.hpp"
#include "avb_mediafoundation_ivf.hpp"
#include "avb_pixel_convert.hpp"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <codecapi.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <strmif.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>

using Microsoft::WRL::ComPtr;

namespace {

LONGLONG seconds_to_hns(double seconds) {
    return (LONGLONG)std::llround(seconds * 1e7);
}

int64_t hns_to_ticks(LONGLONG hns, int timescale) {
    return (int64_t)std::llround((long double)hns * timescale / 1e7L);
}

std::string hr_message(const char *what, HRESULT hr) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s failed: 0x%08lx", what, (unsigned long)hr);
    return buf;
}

HRESULT set_codec_u32(ICodecAPI *api, const GUID &key, UINT32 value) {
    if (!api) return E_NOINTERFACE;
    VARIANT v{};
    v.vt = VT_UI4;
    v.ulVal = value;
    return api->SetValue(&key, &v);
}

HRESULT set_codec_bool(ICodecAPI *api, const GUID &key, bool value) {
    if (!api) return E_NOINTERFACE;
    VARIANT v{};
    v.vt = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    return api->SetValue(&key, &v);
}

// Try each MFT matching the registration until `try_one` accepts one.
// `hardware` selects hardware MFTs only, otherwise software (sync or async).
HRESULT for_each_mft(const GUID &category, const MFT_REGISTER_TYPE_INFO *input,
                     const MFT_REGISTER_TYPE_INFO *output, bool hardware,
                     const std::function<HRESULT(IMFTransform *, bool)> &try_one) {
    const UINT32 flags = hardware
        ? (MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER)
        : (MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_LOCALMFT |
           MFT_ENUM_FLAG_SORTANDFILTER);
    IMFActivate **activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(category, flags, input, output, &activates, &count);
    if (FAILED(hr)) return hr;
    hr = MF_E_TOPO_CODEC_NOT_FOUND;
    for (UINT32 i = 0; i < count; ++i) {
        ComPtr<IMFTransform> candidate;
        HRESULT activate = activates[i]->ActivateObject(IID_PPV_ARGS(&candidate));
        if (FAILED(activate) || !candidate) {
            hr = activate;
            continue;
        }
        ComPtr<IMFAttributes> attributes;
        UINT32 async = FALSE;
        if (SUCCEEDED(candidate->GetAttributes(&attributes)) && attributes)
            attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async);
        if (async && (!attributes || FAILED(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE)))) {
            activates[i]->ShutdownObject();
            continue;
        }
        hr = try_one(candidate.Get(), async != FALSE);
        if (SUCCEEDED(hr)) break;
        activates[i]->ShutdownObject();
    }
    for (UINT32 i = 0; i < count; ++i) {
        if (activates[i]) activates[i]->Release();
    }
    CoTaskMemFree(activates);
    return hr;
}

// Feeds an MFT and collects its output samples, for synchronous MFTs
// (ProcessInput / ProcessOutput) and asynchronous ones (METransformNeedInput /
// METransformHaveOutput events) alike.
//
// A synchronous MFT's output is pulled one sample at a time as it is asked
// for (poll), and on input only when the MFT will not take more without it.
// An MFT that provides its own samples -- a DXVA decoder handing out D3D11
// surfaces -- has a fixed pool of them, and while they wait unreleased here
// ProcessOutput blocks until one comes back: pulling everything it has ready
// (a reordering decoder releases a burst, a drain all of its pictures)
// would never return.
class MfPump {
public:
    ComPtr<IMFTransform> mft;
    ComPtr<IMFMediaEventGenerator> events;
    bool async = false;
    DWORD out_size = 0;
    DWORD out_flags = 0;
    int need_input = 0;
    bool draining = false;
    bool drained = false;
    std::deque<ComPtr<IMFSample>> outputs;
    // Renegotiates the output type on MF_E_TRANSFORM_STREAM_CHANGE.
    std::function<HRESULT()> on_stream_change;

    HRESULT attach(IMFTransform *transform, bool is_async) {
        mft = transform;
        async = is_async;
        if (async) {
            HRESULT hr = mft.As(&events);
            if (FAILED(hr)) return hr;
        }
        return S_OK;
    }

    void refresh_info() {
        MFT_OUTPUT_STREAM_INFO info{};
        if (mft && SUCCEEDED(mft->GetOutputStreamInfo(0, &info))) {
            out_size = info.cbSize;
            out_flags = info.dwFlags;
        }
    }

    HRESULT start() {
        refresh_info();
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        return mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }

    void shutdown() {
        if (!mft) return;
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        if (async) {
            ComPtr<IMFShutdown> shut;
            if (SUCCEEDED(mft.As(&shut))) shut->Shutdown();
        }
        events.Reset();
        mft.Reset();
        outputs.clear();
    }

    HRESULT process_output() {
        for (int attempt = 0; attempt < 4; ++attempt) {
            MFT_OUTPUT_DATA_BUFFER output{};
            ComPtr<IMFSample> allocated;
            if ((out_flags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                              MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) == 0) {
                ComPtr<IMFMediaBuffer> buffer;
                HRESULT hr = MFCreateMemoryBuffer(out_size > 0 ? out_size : (1u << 21), &buffer);
                if (SUCCEEDED(hr)) hr = MFCreateSample(&allocated);
                if (SUCCEEDED(hr)) hr = allocated->AddBuffer(buffer.Get());
                if (FAILED(hr)) return hr;
                output.pSample = allocated.Get();
            }
            DWORD status = 0;
            HRESULT hr = mft->ProcessOutput(0, 1, &output, &status);
            if (output.pEvents) output.pEvents->Release();
            ComPtr<IMFSample> produced;
            if (output.pSample && output.pSample != allocated.Get()) produced.Attach(output.pSample);
            else produced = allocated;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                const HRESULT change = on_stream_change ? on_stream_change() : E_FAIL;
                if (FAILED(change)) return change;
                refresh_info();
                continue;
            }
            if (FAILED(hr)) return hr;
            if (produced) outputs.push_back(produced);
            return S_OK;
        }
        return MF_E_TRANSFORM_STREAM_CHANGE;
    }

    // Synchronous: one sample into `outputs` if the MFT has one. Draining,
    // NEED_MORE_INPUT means it has none left.
    HRESULT pull_sync() {
        const HRESULT hr = process_output();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (draining) drained = true;
            return S_OK;
        }
        return hr;
    }

    HRESULT handle_event(IMFMediaEvent *event) {
        HRESULT status = S_OK;
        event->GetStatus(&status);
        if (FAILED(status)) return status;
        MediaEventType type = MEUnknown;
        event->GetType(&type);
        if (type == METransformNeedInput) {
            ++need_input;
        } else if (type == METransformHaveOutput) {
            const HRESULT hr = process_output();
            if (FAILED(hr) && hr != MF_E_TRANSFORM_NEED_MORE_INPUT) return hr;
        } else if (type == METransformDrainComplete) {
            drained = true;
        }
        return S_OK;
    }

    // Handle queued events. With `wait`, keep waiting (bounded) until `done`.
    HRESULT pump(bool wait, DWORD timeout_ms, const std::function<bool()> &done) {
        const ULONGLONG deadline = GetTickCount64() + timeout_ms;
        for (;;) {
            if (done && done()) return S_OK;
            ComPtr<IMFMediaEvent> event;
            HRESULT hr = events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
            if (hr == MF_E_NO_EVENTS_AVAILABLE) {
                if (!wait) return S_OK;
                if (GetTickCount64() >= deadline) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
                Sleep(1);
                continue;
            }
            if (FAILED(hr)) return hr;
            hr = handle_event(event.Get());
            if (FAILED(hr)) return hr;
        }
    }

    HRESULT send(IMFSample *sample) {
        if (async) {
            HRESULT hr = pump(true, 5000, [this] { return need_input > 0; });
            if (FAILED(hr)) return hr;
            hr = mft->ProcessInput(0, sample, 0);
            if (FAILED(hr)) return hr;
            --need_input;
            return S_OK;
        }
        // Refused input means output is waiting: take as much of it as that
        // takes, and leave the rest in the MFT for poll().
        for (int attempt = 0; attempt < 64; ++attempt) {
            HRESULT hr = mft->ProcessInput(0, sample, 0);
            if (hr != MF_E_NOTACCEPTING) return hr;
            hr = process_output();
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) continue; // it changed its mind
            if (FAILED(hr)) return hr;
        }
        return MF_E_NOTACCEPTING;
    }

    // Before reading `outputs`: an asynchronous MFT's queued events, or the
    // next sample of a synchronous one when none is waiting here.
    HRESULT poll() {
        if (async) return pump(false, 0, nullptr);
        if (!outputs.empty() || drained) return S_OK;
        return pull_sync();
    }

    HRESULT begin_drain() {
        draining = true;
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        return mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    }

    // While draining: wait until an output is queued or the drain completes.
    HRESULT wait_drain() {
        if (!async) return S_OK;
        return pump(true, 5000, [this] { return !outputs.empty() || drained; });
    }

    HRESULT flush() {
        const HRESULT hr = mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        outputs.clear();
        draining = drained = false;
        if (async) need_input = 0;
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return hr;
    }
};

bool contiguous_bytes(IMFSample *sample, std::vector<uint8_t> &out) {
    out.clear();
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || !buffer) return false;
    BYTE *data = nullptr;
    DWORD length = 0;
    if (FAILED(buffer->Lock(&data, nullptr, &length))) return false;
    if (data && length > 0) out.assign(data, data + length);
    buffer->Unlock();
    return true;
}

HRESULT memory_sample(const void *data, size_t size, ComPtr<IMFSample> &sample) {
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer((DWORD)size, &buffer);
    BYTE *dst = nullptr;
    if (SUCCEEDED(hr)) hr = buffer->Lock(&dst, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        std::memcpy(dst, data, size);
        buffer->Unlock();
        hr = buffer->SetCurrentLength((DWORD)size);
    }
    if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer.Get());
    return hr;
}

UINT32 mf_profile(avb_video_codec codec, avb_video_profile profile) {
    switch (codec) {
        case AVB_VIDEO_CODEC_H264:
            switch (profile) {
                case AVB_VIDEO_PROFILE_H264_CONSTRAINED_BASELINE: return eAVEncH264VProfile_ConstrainedBase;
                case AVB_VIDEO_PROFILE_H264_BASELINE: return eAVEncH264VProfile_Base;
                case AVB_VIDEO_PROFILE_H264_HIGH: return eAVEncH264VProfile_High;
                default: return eAVEncH264VProfile_Main;
            }
        case AVB_VIDEO_CODEC_HEVC: return eAVEncH265VProfile_Main_420_8;
        case AVB_VIDEO_CODEC_VP9: return eAVEncVP9VProfile_420_8;
        case AVB_VIDEO_CODEC_AV1: return eAVEncAV1VProfile_Main_420_8;
        default: return 0;
    }
}

// The 12 bytes HEAACWAVEINFO adds after WAVEFORMATEX: raw payload, profile
// level unspecified.
const uint8_t k_heaac_info[12] = {0, 0, 0xfe, 0, 0, 0, 0, 0, 0, 0, 0, 0};

} // namespace

/* ================================================================ encoder */

struct AvbStreamEncoderMediaFoundation::Impl {
    MfStartupScope startup{MFSTARTUP_NOSOCKET};
    MfPump pump;
    ComPtr<ICodecAPI> codec_api;
    ComPtr<ID3D11Device> d3d_device;
    ComPtr<IMFDXGIDeviceManager> device_manager;

    bool is_video = false;
    AvbStreamVideoEncodeConfig video;
    AvbStreamAudioEncodeConfig audio;
    UINT32 fps_num = 30, fps_den = 1;
    bool input_nv12 = true;
    bool bt709 = false;
    bool flushing = false;
    int64_t audio_samples = 0;
    std::vector<uint8_t> stage;
    std::vector<int16_t> audio_stage;
    std::vector<uint8_t> bytes;

    ~Impl() {
        codec_api.Reset();
        pump.shutdown();
    }
};

AvbStreamEncoderMediaFoundation::AvbStreamEncoderMediaFoundation() : m_impl(new Impl()) {}
AvbStreamEncoderMediaFoundation::~AvbStreamEncoderMediaFoundation() = default;

const char *AvbStreamEncoderMediaFoundation::get_last_error() const {
    return m_last_error.empty() ? nullptr : m_last_error.c_str();
}

avb_result AvbStreamEncoderMediaFoundation::open_video(const AvbStreamVideoEncodeConfig &config) {
    Impl &m = *m_impl;
    if (!m.startup.started()) {
        m_last_error = "MFStartup failed.";
        return AVB_ERROR_BACKEND_NOT_AVAILABLE;
    }
    const avb_video_stream_encode_params &p = config.params;
    const avb_video_encode_params &v = p.video;
    m.is_video = true;
    m.video = config;

    const bool external = v.input_memory == AVB_VIDEO_MEMORY_EXTERNAL;
    if (v.input_memory == AVB_VIDEO_MEMORY_BACKEND_NATIVE ||
        (external && v.input_external_type != AVB_VIDEO_EXTERNAL_D3D11_TEXTURE)) {
        m_last_error = "Media Foundation stream encoding takes CPU frames or D3D11 NV12 textures.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    if (external && !v.hardware_context) {
        m_last_error = "D3D11 texture input requires hardware_context to be the texture's ID3D11Device.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    if (external && v.input_format != AVB_PIXEL_FORMAT_UNKNOWN && v.input_format != AVB_PIXEL_FORMAT_NV12) {
        m_last_error = "D3D11 texture input must be NV12.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    const GUID subtype = mf_encode_video_subtype(v.codec, 0);
    if (IsEqualGUID(subtype, GUID_NULL)) {
        m_last_error = "Unsupported video codec for Media Foundation.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    MFAverageTimePerFrameToFrameRate(seconds_to_hns(1.0 / v.frame_rate), &m.fps_num, &m.fps_den);
    if (m.fps_num == 0 || m.fps_den == 0) { m.fps_num = 30; m.fps_den = 1; }
    m.bt709 = avb::detail::use_bt709(v.width, v.height);

    const UINT32 w = (UINT32)v.width, h = (UINT32)v.height;
    std::string failure;
    auto try_one = [&](IMFTransform *transform, bool async) -> HRESULT {
        MfPump pump;
        HRESULT hr = pump.attach(transform, async);
        if (FAILED(hr)) return hr;
        if (external) {
            if (!m.device_manager) {
                hr = mf_create_d3d11_device_manager(static_cast<ID3D11Device *>(v.hardware_context),
                                                    &m.d3d_device, &m.device_manager);
                if (FAILED(hr)) { failure = hr_message("Creating the D3D11 device manager", hr); return hr; }
            }
            hr = transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)m.device_manager.Get());
            if (FAILED(hr)) { failure = hr_message("MFT_MESSAGE_SET_D3D_MANAGER", hr); return hr; }
        }
        ComPtr<ICodecAPI> api;
        transform->QueryInterface(IID_PPV_ARGS(&api));
        if (api) {
            if (p.low_latency) set_codec_bool(api.Get(), CODECAPI_AVLowLatencyMode, true);
            if (p.max_b_frames >= 0)
                set_codec_u32(api.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, (UINT32)p.max_b_frames);
            if (p.gop_size > 0) set_codec_u32(api.Get(), CODECAPI_AVEncMPVGOPSize, (UINT32)p.gop_size);
            if (p.rate_control == AVB_RATE_CONTROL_CBR) {
                set_codec_u32(api.Get(), CODECAPI_AVEncCommonRateControlMode,
                              eAVEncCommonRateControlMode_CBR);
            } else if (p.rate_control == AVB_RATE_CONTROL_VBR) {
                set_codec_u32(api.Get(), CODECAPI_AVEncCommonRateControlMode,
                              p.max_bitrate > 0 ? eAVEncCommonRateControlMode_PeakConstrainedVBR
                                                : eAVEncCommonRateControlMode_UnconstrainedVBR);
            }
            set_codec_u32(api.Get(), CODECAPI_AVEncCommonMeanBitRate, (UINT32)v.bitrate);
            if (p.max_bitrate > 0)
                set_codec_u32(api.Get(), CODECAPI_AVEncCommonMaxBitRate, (UINT32)p.max_bitrate);
        }

        auto make_output = [&](UINT32 profile, ComPtr<IMFMediaType> &type) {
            MFCreateMediaType(&type);
            type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            type->SetGUID(MF_MT_SUBTYPE, subtype);
            type->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)v.bitrate);
            type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            if (profile) type->SetUINT32(MF_MT_MPEG2_PROFILE, profile);
            MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, w, h);
            MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, m.fps_num, m.fps_den);
            MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        };
        ComPtr<IMFMediaType> in_type;
        MFCreateMediaType(&in_type);
        in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(in_type.Get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(in_type.Get(), MF_MT_FRAME_RATE, m.fps_num, m.fps_den);
        MFSetAttributeRatio(in_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        in_type->SetUINT32(MF_MT_DEFAULT_STRIDE, w);
        in_type->SetUINT32(MF_MT_YUV_MATRIX, m.bt709 ? MFVideoTransferMatrix_BT709
                                                     : MFVideoTransferMatrix_BT601);
        in_type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);

        const UINT32 profile = mf_profile(v.codec, p.profile);
        bool nv12 = true;
        auto set_types = [&](UINT32 prof) -> HRESULT {
            ComPtr<IMFMediaType> out_type;
            make_output(prof, out_type);
            HRESULT r = mf_encode_select_video_output_type(transform, out_type.Get(), subtype,
                                                           w, h, m.fps_num, m.fps_den);
            if (SUCCEEDED(r))
                return mf_encode_select_video_input_type(transform, in_type.Get(), w, h,
                                                         m.fps_num, m.fps_den, &nv12);
            r = mf_encode_select_video_input_type(transform, in_type.Get(), w, h,
                                                  m.fps_num, m.fps_den, &nv12);
            if (FAILED(r)) return r;
            return mf_encode_select_video_output_type(transform, out_type.Get(), subtype,
                                                      w, h, m.fps_num, m.fps_den);
        };
        hr = set_types(profile);
        if (FAILED(hr) && profile == (UINT32)eAVEncH264VProfile_ConstrainedBase)
            hr = set_types(eAVEncH264VProfile_Base);
        if (FAILED(hr)) { failure = hr_message("Configuring the video encoder media types", hr); return hr; }

        hr = pump.start();
        if (FAILED(hr)) { failure = hr_message("Starting the video encoder", hr); return hr; }

        ComPtr<IMFMediaType> current;
        if (SUCCEEDED(transform->GetOutputCurrentType(0, &current)) && current) {
            UINT32 size = 0;
            if (SUCCEEDED(current->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size > 0) {
                m_output.extradata.resize(size);
                current->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, m_output.extradata.data(), size, nullptr);
            }
        }
        m.pump = std::move(pump);
        m.codec_api = api;
        m.input_nv12 = nv12;
        return S_OK;
    };

    std::vector<bool> passes;
    if (external || v.hardware_policy == AVB_HARDWARE_REQUIRE) passes = {true};
    else if (v.hardware_policy == AVB_HARDWARE_PREFER) passes = {true, false};
    else passes = {false};

    const MFT_REGISTER_TYPE_INFO out_info{MFMediaType_Video, subtype};
    HRESULT hr = MF_E_TOPO_CODEC_NOT_FOUND;
    for (bool hardware : passes) {
        hr = for_each_mft(MFT_CATEGORY_VIDEO_ENCODER, nullptr, &out_info, hardware, try_one);
        if (SUCCEEDED(hr)) {
            m_output.hardware_device = hardware ? AVB_HW_DEVICE_D3D11VA : AVB_HW_DEVICE_AUTO;
            break;
        }
    }
    if (FAILED(hr)) {
        m_last_error = failure.empty()
            ? hr_message("Finding a Media Foundation video encoder", hr) : failure;
        return AVB_ERROR_OPEN_FAILED;
    }
    m_output.length_prefixed = false;
    return AVB_OK;
}

avb_result AvbStreamEncoderMediaFoundation::open_audio(const AvbStreamAudioEncodeConfig &config) {
    Impl &m = *m_impl;
    if (!m.startup.started()) {
        m_last_error = "MFStartup failed.";
        return AVB_ERROR_BACKEND_NOT_AVAILABLE;
    }
    const avb_audio_encode_params &a = config.params.audio;
    m.audio = config;
    if (a.codec != AVB_AUDIO_CODEC_AAC) {
        m_last_error = "Media Foundation has no Opus encoder; use the FFmpeg or GStreamer backend.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    if ((a.sample_rate != 44100 && a.sample_rate != 48000) ||
        (a.channels != 1 && a.channels != 2 && a.channels != 6)) {
        m_last_error = "The Media Foundation AAC encoder takes 44100/48000 Hz with 1, 2 or 6 channels.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }

    std::string failure;
    auto try_one = [&](IMFTransform *transform, bool async) -> HRESULT {
        MfPump pump;
        HRESULT hr = pump.attach(transform, async);
        if (FAILED(hr)) return hr;
        ComPtr<IMFMediaType> out_type, in_type;
        MFCreateMediaType(&out_type);
        out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        out_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        out_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        out_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)a.sample_rate);
        out_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)a.channels);
        out_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, mf_encode_aac_bytes_per_sec(a.bitrate));
        out_type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        out_type->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
        MFCreateMediaType(&in_type);
        in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        in_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        in_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        in_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)a.sample_rate);
        in_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)a.channels);
        in_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, (UINT32)a.channels * 2);
        in_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)(a.sample_rate * a.channels * 2));
        hr = transform->SetOutputType(0, out_type.Get(), 0);
        if (SUCCEEDED(hr)) {
            hr = transform->SetInputType(0, in_type.Get(), 0);
        } else {
            hr = transform->SetInputType(0, in_type.Get(), 0);
            if (SUCCEEDED(hr)) hr = transform->SetOutputType(0, out_type.Get(), 0);
        }
        if (FAILED(hr)) { failure = hr_message("Configuring the AAC encoder media types", hr); return hr; }
        hr = pump.start();
        if (FAILED(hr)) { failure = hr_message("Starting the AAC encoder", hr); return hr; }
        ComPtr<IMFMediaType> current;
        if (SUCCEEDED(transform->GetOutputCurrentType(0, &current)) && current) {
            UINT32 size = 0;
            if (SUCCEEDED(current->GetBlobSize(MF_MT_USER_DATA, &size)) && size > 12) {
                std::vector<uint8_t> user(size);
                current->GetBlob(MF_MT_USER_DATA, user.data(), size, nullptr);
                m_output.extradata.assign(user.begin() + 12, user.end());
            }
        }
        m.pump = std::move(pump);
        return S_OK;
    };
    const MFT_REGISTER_TYPE_INFO in_info{MFMediaType_Audio, MFAudioFormat_PCM};
    const MFT_REGISTER_TYPE_INFO out_info{MFMediaType_Audio, MFAudioFormat_AAC};
    const HRESULT hr = for_each_mft(MFT_CATEGORY_AUDIO_ENCODER, &in_info, &out_info, false, try_one);
    if (FAILED(hr)) {
        m_last_error = failure.empty() ? hr_message("Finding the AAC encoder MFT", hr) : failure;
        return AVB_ERROR_OPEN_FAILED;
    }
    return AVB_OK;
}

avb_result AvbStreamEncoderMediaFoundation::send_video(const avb_video_frame &frame, double pts_sec,
                                                       bool force_keyframe) {
    Impl &m = *m_impl;
    if (!m.is_video || !m.pump.mft) return AVB_ERROR_INVALID_ARGUMENT;
    const avb_video_encode_params &v = m.video.params.video;
    if (force_keyframe && m.codec_api)
        set_codec_u32(m.codec_api.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);

    ComPtr<IMFSample> sample;
    HRESULT hr = S_OK;
    if (v.input_memory == AVB_VIDEO_MEMORY_EXTERNAL) {
        if (!frame.native_handle || frame.external_type != AVB_VIDEO_EXTERNAL_D3D11_TEXTURE) {
            m_last_error = "D3D11 texture input requires an ID3D11Texture2D native_handle.";
            return AVB_ERROR_INVALID_ARGUMENT;
        }
        auto *texture = static_cast<ID3D11Texture2D *>(frame.native_handle);
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if ((int)desc.Width != v.width || (int)desc.Height != v.height ||
            desc.Format != DXGI_FORMAT_NV12 || frame.native_handle_id >= desc.ArraySize) {
            m_last_error = "D3D11 texture must match the configured NV12 size and subresource.";
            return AVB_ERROR_INVALID_ARGUMENT;
        }
        ComPtr<IMFMediaBuffer> buffer;
        hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture,
                                       (UINT)frame.native_handle_id, FALSE, &buffer);
        if (SUCCEEDED(hr))
            hr = buffer->SetCurrentLength((DWORD)((size_t)v.width * v.height * 3 / 2));
        if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
        if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer.Get());
    } else {
        avb::detail::convert_to_yuv420(frame, m.input_nv12, m.bt709, m.stage);
        hr = memory_sample(m.stage.data(), m.stage.size(), sample);
    }
    if (FAILED(hr)) {
        m_last_error = hr_message("Creating the video input sample", hr);
        return AVB_ERROR_ENCODE_FAILED;
    }
    sample->SetSampleTime(seconds_to_hns(pts_sec));
    sample->SetSampleDuration(seconds_to_hns(1.0 / v.frame_rate));
    hr = m.pump.send(sample.Get());
    if (FAILED(hr)) {
        m_last_error = hr_message("Encoding the video frame", hr);
        return AVB_ERROR_ENCODE_FAILED;
    }
    return AVB_OK;
}

avb_result AvbStreamEncoderMediaFoundation::send_audio(const float *interleaved, int frames) {
    Impl &m = *m_impl;
    if (m.is_video || !m.pump.mft) return AVB_ERROR_INVALID_ARGUMENT;
    const avb_audio_encode_params &a = m.audio.params.audio;
    const MfEncodeAudioData pcm =
        mf_encode_pack_audio_f32(interleaved, frames, a.channels, false, m.audio_stage);
    ComPtr<IMFSample> sample;
    HRESULT hr = memory_sample(pcm.data, pcm.size, sample);
    if (SUCCEEDED(hr)) {
        sample->SetSampleTime(seconds_to_hns((double)m.audio_samples / a.sample_rate));
        sample->SetSampleDuration(seconds_to_hns((double)frames / a.sample_rate));
        hr = m.pump.send(sample.Get());
    }
    if (FAILED(hr)) {
        m_last_error = hr_message("Encoding audio", hr);
        return AVB_ERROR_ENCODE_FAILED;
    }
    m.audio_samples += frames;
    return AVB_OK;
}

avb_result AvbStreamEncoderMediaFoundation::receive(AvbStreamEncodedPacket &out) {
    Impl &m = *m_impl;
    if (!m.pump.mft) return AVB_ERROR_INVALID_ARGUMENT;
    const int timescale = m.is_video ? AVB_STREAM_VIDEO_TIMESCALE : m.audio.params.audio.sample_rate;
    for (int round = 0; round < 64; ++round) {
        HRESULT hr = m.pump.poll();
        if (FAILED(hr)) {
            m_last_error = hr_message("Polling the encoder", hr);
            return AVB_ERROR_ENCODE_FAILED;
        }
        while (!m.pump.outputs.empty()) {
            ComPtr<IMFSample> sample = m.pump.outputs.front();
            m.pump.outputs.pop_front();
            if (!contiguous_bytes(sample.Get(), m.bytes) || m.bytes.empty()) continue;
            out.data.swap(m.bytes);
            LONGLONG time = 0, duration = 0;
            out.pts = SUCCEEDED(sample->GetSampleTime(&time)) ? hns_to_ticks(time, timescale) : -1;
            out.duration = SUCCEEDED(sample->GetSampleDuration(&duration)) && duration > 0
                ? hns_to_ticks(duration, timescale) : -1;
            UINT64 dts = 0;
            out.dts = SUCCEEDED(sample->GetUINT64(MFSampleExtension_DecodeTimestamp, &dts))
                ? hns_to_ticks((LONGLONG)dts, timescale) : -1;
            out.keyframe = MFGetAttributeUINT32(sample.Get(), MFSampleExtension_CleanPoint, 0) != 0;
            return AVB_OK;
        }
        if (!m.flushing) return AVB_ERROR_AGAIN;
        if (m.pump.drained) return AVB_ERROR_EOF;
        hr = m.pump.wait_drain();
        if (FAILED(hr)) {
            m_last_error = hr_message("Draining the encoder", hr);
            return AVB_ERROR_ENCODE_FAILED;
        }
    }
    return AVB_ERROR_AGAIN;
}

avb_result AvbStreamEncoderMediaFoundation::set_bitrate(int bitrate) {
    Impl &m = *m_impl;
    if (!m.is_video) {
        m_last_error = "The Media Foundation AAC encoder cannot change its bitrate while encoding.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    const HRESULT hr = set_codec_u32(m.codec_api.Get(), CODECAPI_AVEncCommonMeanBitRate, (UINT32)bitrate);
    if (FAILED(hr)) {
        m_last_error = hr_message("Changing the encoder bitrate (CODECAPI_AVEncCommonMeanBitRate)", hr);
        return AVB_ERROR_ENCODE_FAILED;
    }
    return AVB_OK;
}

avb_result AvbStreamEncoderMediaFoundation::flush() {
    Impl &m = *m_impl;
    if (!m.pump.mft) return AVB_ERROR_INVALID_ARGUMENT;
    m.flushing = true;
    const HRESULT hr = m.pump.begin_drain();
    if (FAILED(hr)) {
        m_last_error = hr_message("Draining the encoder", hr);
        return AVB_ERROR_ENCODE_FAILED;
    }
    return AVB_OK;
}

/* ================================================================ decoder */

struct AvbStreamDecoderMediaFoundation::Impl {
    MfStartupScope startup{MFSTARTUP_NOSOCKET};
    MfPump pump;
    AvbStreamDecodeConfig config;
    bool is_video = false;
    bool configured = false;
    GUID subtype = GUID_NULL;
    ComPtr<ID3D11Device> d3d_device;
    ComPtr<IMFDXGIDeviceManager> device_manager;
    bool native_output = false;
    avb_hardware_device hw = AVB_HW_DEVICE_AUTO;

    int coded_width = 0, coded_height = 0;
    int width = 0, height = 0;
    int stride = 0;
    bool bt709 = false;
    bool full_range = false;
    bool color_known = false;
    int64_t input_index = 0;
    std::vector<uint8_t> nv12;
    std::vector<uint8_t> frame_buf;
    std::map<void *, ComPtr<IMFSample>> retained;

    int sample_rate = 0, channels = 0, bits = 16;
    bool out_float = false;
    std::vector<uint8_t> bytes;

    ~Impl() {
        retained.clear();
        pump.shutdown();
    }

    HRESULT select_video_output() {
        int w = 0, h = 0;
        HRESULT hr = mf_ivf_select_decoder_output(pump.mft.Get(), &w, &h, 0, 0,
                                                  &pump.out_size, &pump.out_flags);
        if (FAILED(hr)) return hr;
        ComPtr<IMFMediaType> type;
        hr = pump.mft->GetOutputCurrentType(0, &type);
        if (FAILED(hr)) return hr;
        UINT32 cw = 0, ch = 0;
        MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &cw, &ch);
        coded_width = (int)cw;
        coded_height = (int)ch;
        width = coded_width;
        height = coded_height;
        MFVideoArea area{};
        if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8 *)&area, sizeof(area), nullptr)) &&
            area.Area.cx > 0 && area.Area.cy > 0 &&
            area.Area.cx <= (LONG)cw && area.Area.cy <= (LONG)ch) {
            width = (int)area.Area.cx;
            height = (int)area.Area.cy;
        }
        UINT32 default_stride = 0;
        stride = SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &default_stride)) &&
                         (int)default_stride > 0
                     ? (int)default_stride : coded_width;
        UINT32 matrix = 0, range = 0;
        color_known = SUCCEEDED(type->GetUINT32(MF_MT_YUV_MATRIX, &matrix));
        bt709 = color_known ? matrix == MFVideoTransferMatrix_BT709
                            : avb::detail::use_bt709(width, height);
        full_range = SUCCEEDED(type->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range)) &&
                     range == MFNominalRange_0_255;
        return S_OK;
    }

    HRESULT select_audio_output() {
        HRESULT result = MF_E_INVALIDMEDIATYPE;
        for (int pass = 0; pass < 2 && FAILED(result); ++pass) {
            const GUID want = pass == 0 ? MFAudioFormat_Float : MFAudioFormat_PCM;
            for (DWORD i = 0;; ++i) {
                ComPtr<IMFMediaType> type;
                HRESULT hr = pump.mft->GetOutputAvailableType(0, i, &type);
                if (hr == MF_E_NO_MORE_TYPES) break;
                if (FAILED(hr) || !type) continue;
                GUID sub = GUID_NULL;
                type->GetGUID(MF_MT_SUBTYPE, &sub);
                UINT32 bps = 0;
                type->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bps);
                if (!IsEqualGUID(sub, want) || (pass == 1 && bps != 16) || (pass == 0 && bps != 32)) continue;
                if (FAILED(pump.mft->SetOutputType(0, type.Get(), 0))) continue;
                UINT32 rate = 0, chans = 0;
                type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
                type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &chans);
                sample_rate = (int)rate;
                channels = (int)chans;
                bits = (int)bps;
                out_float = pass == 0;
                result = S_OK;
                break;
            }
        }
        pump.refresh_info();
        return result;
    }
};

AvbStreamDecoderMediaFoundation::AvbStreamDecoderMediaFoundation() : m_impl(new Impl()) {}
AvbStreamDecoderMediaFoundation::~AvbStreamDecoderMediaFoundation() = default;

const char *AvbStreamDecoderMediaFoundation::get_last_error() const {
    return m_last_error.empty() ? nullptr : m_last_error.c_str();
}

void AvbStreamDecoderMediaFoundation::audio_format(int &sample_rate, int &channels) const {
    sample_rate = m_impl->sample_rate;
    channels = m_impl->channels;
}

avb_hardware_device AvbStreamDecoderMediaFoundation::hardware_device() const {
    return m_impl->hw;
}

avb_result AvbStreamDecoderMediaFoundation::configure_video(const std::vector<uint8_t> &config) {
    Impl &m = *m_impl;
    const avb_stream_decode_params &p = m.config.params;
    int w = 0, h = 0;
    if (avb::detail::is_h26x(p.video_codec) && !config.empty()) {
        avb::detail::ParamSets ps(p.video_codec);
        ps.load_extradata(config.data(), config.size());
        if (!ps.sps().empty()) {
            const auto &sps = ps.sps().front();
            if (p.video_codec == AVB_VIDEO_CODEC_HEVC)
                avb::detail::hevc_sps_dimensions(sps.data(), sps.size(), w, h);
            else
                avb::detail::h264_sps_dimensions(sps.data(), sps.size(), w, h);
        }
    }
    const bool want_d3d = m.native_output || p.hardware_policy != AVB_HARDWARE_DISABLED;

    std::string failure;
    auto try_one = [&](IMFTransform *transform, bool async) -> HRESULT {
        MfPump pump;
        HRESULT hr = pump.attach(transform, async);
        if (FAILED(hr)) return hr;
        bool d3d = false;
        if (want_d3d) {
            if (!m.device_manager) {
                hr = mf_create_d3d11_device_manager(static_cast<ID3D11Device *>(p.hardware_context),
                                                    &m.d3d_device, &m.device_manager);
                if (FAILED(hr) && (m.native_output || p.hardware_policy == AVB_HARDWARE_REQUIRE)) {
                    failure = hr_message("Creating the D3D11 device manager", hr);
                    return hr;
                }
            }
            if (m.device_manager) {
                hr = transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                               (ULONG_PTR)m.device_manager.Get());
                d3d = SUCCEEDED(hr);
                if (!d3d && (m.native_output || p.hardware_policy == AVB_HARDWARE_REQUIRE)) {
                    failure = hr_message("Enabling hardware decoding (MFT_MESSAGE_SET_D3D_MANAGER)", hr);
                    return hr;
                }
            }
        }
        if (p.low_latency) {
            ComPtr<IMFAttributes> attributes;
            if (SUCCEEDED(transform->GetAttributes(&attributes)) && attributes)
                attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
            ComPtr<ICodecAPI> api;
            if (SUCCEEDED(transform->QueryInterface(IID_PPV_ARGS(&api))))
                set_codec_bool(api.Get(), CODECAPI_AVLowLatencyMode, true);
        }
        ComPtr<IMFMediaType> in_type;
        MFCreateMediaType(&in_type);
        in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in_type->SetGUID(MF_MT_SUBTYPE, m.subtype);
        in_type->SetUINT32(MF_MT_INTERLACE_MODE, avb::detail::is_h26x(p.video_codec)
                               ? MFVideoInterlace_MixedInterlaceOrProgressive
                               : MFVideoInterlace_Progressive);
        if (w > 0 && h > 0) MFSetAttributeSize(in_type.Get(), MF_MT_FRAME_SIZE, (UINT32)w, (UINT32)h);
        hr = transform->SetInputType(0, in_type.Get(), 0);
        if (FAILED(hr)) { failure = hr_message("Setting the video decoder input type", hr); return hr; }

        m.pump = std::move(pump);
        Impl *self = &m;
        m.pump.on_stream_change = [self] { return self->select_video_output(); };
        // Some decoders only offer output types once they have seen the stream;
        // those report a stream change on their first output instead.
        m.select_video_output();
        hr = m.pump.start();
        if (FAILED(hr)) {
            failure = hr_message("Starting the video decoder", hr);
            m.pump = MfPump();
            return hr;
        }
        m.hw = d3d ? AVB_HW_DEVICE_D3D11VA : AVB_HW_DEVICE_AUTO;
        return S_OK;
    };

    const MFT_REGISTER_TYPE_INFO in_info{MFMediaType_Video, m.subtype};
    HRESULT hr = MF_E_TOPO_CODEC_NOT_FOUND;
    std::vector<bool> passes = p.hardware_policy == AVB_HARDWARE_DISABLED
        ? std::vector<bool>{false} : std::vector<bool>{true, false};
    for (bool hardware : passes) {
        hr = for_each_mft(MFT_CATEGORY_VIDEO_DECODER, &in_info, nullptr, hardware, try_one);
        if (SUCCEEDED(hr)) break;
    }
    if (FAILED(hr)) {
        m_last_error = failure.empty() ? hr_message("Finding a Media Foundation video decoder", hr) : failure;
        return AVB_ERROR_OPEN_FAILED;
    }
    m.configured = true;
    return AVB_OK;
}

avb_result AvbStreamDecoderMediaFoundation::open(const AvbStreamDecodeConfig &config) {
    Impl &m = *m_impl;
    if (!m.startup.started()) {
        m_last_error = "MFStartup failed.";
        return AVB_ERROR_BACKEND_NOT_AVAILABLE;
    }
    m.config = config;
    const avb_stream_decode_params &p = config.params;
    m.is_video = p.is_video != 0;

    if (m.is_video) {
        if (p.video_memory == AVB_VIDEO_MEMORY_BACKEND_NATIVE ||
            (p.video_memory == AVB_VIDEO_MEMORY_EXTERNAL &&
             p.video_external_type != AVB_VIDEO_EXTERNAL_D3D11_TEXTURE)) {
            m_last_error = "Media Foundation stream decoding outputs CPU frames or D3D11 textures.";
            return AVB_ERROR_INVALID_ARGUMENT;
        }
        m.native_output = p.video_memory == AVB_VIDEO_MEMORY_EXTERNAL;
        if (m.native_output && p.video_format != AVB_PIXEL_FORMAT_UNKNOWN &&
            p.video_format != AVB_PIXEL_FORMAT_NV12) {
            m_last_error = "D3D11 texture output is NV12.";
            return AVB_ERROR_INVALID_ARGUMENT;
        }
        switch (p.video_codec) {
            case AVB_VIDEO_CODEC_H264: m.subtype = MFVideoFormat_H264; break;
            case AVB_VIDEO_CODEC_HEVC: m.subtype = MFVideoFormat_HEVC; break;
            default: m.subtype = mf_ivf_codec_subtype(p.video_codec); break;
        }
        if (IsEqualGUID(m.subtype, GUID_NULL)) {
            m_last_error = "Unsupported video codec for Media Foundation.";
            return AVB_ERROR_INVALID_ARGUMENT;
        }
        // H.264/HEVC without configuration wait for the first keyframe, whose
        // SPS gives the decoder its input size.
        if (!avb::detail::is_h26x(p.video_codec) || !config.extradata.empty())
            return configure_video(config.extradata);
        return AVB_OK;
    }

    if (p.audio_codec != AVB_AUDIO_CODEC_AAC) {
        m_last_error = "Media Foundation has no Opus decoder; use the FFmpeg or GStreamer backend.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    std::string failure;
    auto try_one = [&](IMFTransform *transform, bool async) -> HRESULT {
        MfPump pump;
        HRESULT hr = pump.attach(transform, async);
        if (FAILED(hr)) return hr;
        ComPtr<IMFMediaType> in_type;
        MFCreateMediaType(&in_type);
        in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        in_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        in_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)p.sample_rate);
        in_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)p.channels);
        in_type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        std::vector<uint8_t> user(k_heaac_info, k_heaac_info + 12);
        user.insert(user.end(), config.extradata.begin(), config.extradata.end());
        in_type->SetBlob(MF_MT_USER_DATA, user.data(), (UINT32)user.size());
        hr = transform->SetInputType(0, in_type.Get(), 0);
        if (FAILED(hr)) { failure = hr_message("Setting the AAC decoder input type", hr); return hr; }
        m.pump = std::move(pump);
        Impl *self = &m;
        m.pump.on_stream_change = [self] { return self->select_audio_output(); };
        hr = m.select_audio_output();
        if (SUCCEEDED(hr)) hr = m.pump.start();
        if (FAILED(hr)) {
            failure = hr_message("Configuring the AAC decoder output", hr);
            m.pump = MfPump();
            return hr;
        }
        return S_OK;
    };
    const MFT_REGISTER_TYPE_INFO in_info{MFMediaType_Audio, MFAudioFormat_AAC};
    const HRESULT hr = for_each_mft(MFT_CATEGORY_AUDIO_DECODER, &in_info, nullptr, false, try_one);
    if (FAILED(hr)) {
        m_last_error = failure.empty() ? hr_message("Finding the AAC decoder MFT", hr) : failure;
        return AVB_ERROR_OPEN_FAILED;
    }
    return AVB_OK;
}

avb_result AvbStreamDecoderMediaFoundation::send(const AvbStreamDecodeInput &input) {
    Impl &m = *m_impl;
    if (!input.data) {
        if (!m.pump.mft) {
            m.pump.draining = m.pump.drained = true;
            return AVB_OK;
        }
        const HRESULT hr = m.pump.begin_drain();
        if (FAILED(hr)) {
            m_last_error = hr_message("Draining the decoder", hr);
            return AVB_ERROR_DECODE_FAILED;
        }
        return AVB_OK;
    }
    if (m.is_video && !m.configured) {
        const avb_result res = configure_video(input.config ? *input.config : std::vector<uint8_t>{});
        if (res != AVB_OK) return res;
    }
    ComPtr<IMFSample> sample;
    HRESULT hr = memory_sample(input.data, input.size, sample);
    if (FAILED(hr)) {
        m_last_error = hr_message("Creating the decoder input sample", hr);
        return AVB_ERROR_DECODE_FAILED;
    }
    double pts = input.pts_sec;
    if (pts < 0.0) {
        pts = m.is_video ? (double)m.input_index / 30.0
                         : (double)m.input_index * 1024.0 / std::max(1, m.sample_rate);
    }
    ++m.input_index;
    sample->SetSampleTime(seconds_to_hns(pts));
    if (input.keyframe) sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    hr = m.pump.send(sample.Get());
    if (FAILED(hr)) {
        m_last_error = hr_message(m.is_video ? "Decoding video" : "Decoding audio", hr);
        return AVB_ERROR_DECODE_FAILED;
    }
    return AVB_OK;
}

avb_result AvbStreamDecoderMediaFoundation::receive_video(avb_video_frame &out) {
    Impl &m = *m_impl;
    if (!m.is_video) return AVB_ERROR_INVALID_ARGUMENT;
    if (!m.pump.mft) return m.pump.drained ? AVB_ERROR_EOF : AVB_ERROR_AGAIN;
    const avb_stream_decode_params &p = m.config.params;
    for (int round = 0; round < 64; ++round) {
        HRESULT hr = m.pump.poll();
        if (FAILED(hr)) {
            m_last_error = hr_message("Polling the decoder", hr);
            return AVB_ERROR_DECODE_FAILED;
        }
        while (!m.pump.outputs.empty()) {
            ComPtr<IMFSample> sample = m.pump.outputs.front();
            m.pump.outputs.pop_front();
            DWORD buffers = 0;
            sample->GetBufferCount(&buffers);
            if (buffers == 0 || m.width <= 0 || m.height <= 0) continue;
            LONGLONG time = 0;
            const double pts = SUCCEEDED(sample->GetSampleTime(&time)) ? (double)time / 1e7 : -1.0;

            if (m.native_output) {
                ComPtr<IMFMediaBuffer> raw;
                ComPtr<IMFDXGIBuffer> dxgi;
                ComPtr<ID3D11Texture2D> texture;
                UINT subresource = 0;
                hr = sample->GetBufferByIndex(0, &raw);
                if (SUCCEEDED(hr)) hr = raw.As(&dxgi);
                if (SUCCEEDED(hr))
                    hr = dxgi->GetResource(__uuidof(ID3D11Texture2D),
                                           reinterpret_cast<void **>(texture.GetAddressOf()));
                if (SUCCEEDED(hr)) hr = dxgi->GetSubresourceIndex(&subresource);
                if (FAILED(hr) || !texture) {
                    m_last_error = "The decoder did not return a D3D11 texture.";
                    return AVB_ERROR_DECODE_FAILED;
                }
                avb::detail::init_cpu_frame(out, m.width, m.height, AVB_PIXEL_FORMAT_NV12, pts);
                out.memory_type = AVB_VIDEO_MEMORY_EXTERNAL;
                out.external_type = AVB_VIDEO_EXTERNAL_D3D11_TEXTURE;
                out.hardware_device = AVB_HW_DEVICE_D3D11VA;
                out.native_handle = texture.Get();
                out.native_handle_id = subresource;
                out.native_owner = sample.Get();
                out.color_range = m.full_range ? AVB_COLOR_RANGE_FULL : AVB_COLOR_RANGE_LIMITED;
                out.color_matrix = m.bt709 ? AVB_COLOR_MATRIX_BT709 : AVB_COLOR_MATRIX_BT601;
                m.retained[sample.Get()] = sample;
                return AVB_OK;
            }

            avb_video_frame nv12{};
            if (mf_decode_copy_cpu_frame(sample.Get(), m.width, m.height,
                                         std::max(m.coded_height, m.height), m.stride, false,
                                         AVB_PIXEL_FORMAT_NV12, pts, m.nv12, nv12) != AVB_OK) {
                m_last_error = "The decoder returned an unreadable NV12 frame.";
                return AVB_ERROR_DECODE_FAILED;
            }
            if (p.video_format == AVB_PIXEL_FORMAT_NV12) {
                out = nv12;
                out.color_range = m.full_range ? AVB_COLOR_RANGE_FULL : AVB_COLOR_RANGE_LIMITED;
                out.color_matrix = m.bt709 ? AVB_COLOR_MATRIX_BT709 : AVB_COLOR_MATRIX_BT601;
            } else {
                avb::detail::Yuv420Planes planes;
                planes.y = nv12.plane_data[0];
                planes.y_stride = nv12.plane_stride[0];
                planes.u = nv12.plane_data[1];
                planes.u_stride = nv12.plane_stride[1];
                planes.nv12 = true;
                avb::detail::yuv420_to_frame(planes, m.width, m.height, p.video_format, m.bt709,
                                             m.full_range, pts, m.frame_buf, out);
            }
            out.hardware_device = m.hw;
            return AVB_OK;
        }
        if (!m.pump.draining) return AVB_ERROR_AGAIN;
        if (m.pump.drained) return AVB_ERROR_EOF;
        hr = m.pump.wait_drain();
        if (FAILED(hr)) {
            m_last_error = hr_message("Draining the decoder", hr);
            return AVB_ERROR_DECODE_FAILED;
        }
    }
    return AVB_ERROR_AGAIN;
}

void AvbStreamDecoderMediaFoundation::release_video(avb_video_frame &frame) {
    if (frame.memory_type == AVB_VIDEO_MEMORY_EXTERNAL && frame.native_owner)
        m_impl->retained.erase(frame.native_owner);
}

avb_result AvbStreamDecoderMediaFoundation::receive_audio(std::vector<float> &out, int &frames,
                                                          double &pts_sec) {
    Impl &m = *m_impl;
    frames = 0;
    pts_sec = -1.0;
    if (m.is_video || !m.pump.mft) return AVB_ERROR_INVALID_ARGUMENT;
    for (int round = 0; round < 64; ++round) {
        HRESULT hr = m.pump.poll();
        if (FAILED(hr)) {
            m_last_error = hr_message("Polling the decoder", hr);
            return AVB_ERROR_DECODE_FAILED;
        }
        while (!m.pump.outputs.empty()) {
            ComPtr<IMFSample> sample = m.pump.outputs.front();
            m.pump.outputs.pop_front();
            if (!contiguous_bytes(sample.Get(), m.bytes) || m.bytes.empty() || m.channels <= 0) continue;
            const size_t bytes_per_frame = (size_t)m.channels * (m.bits / 8);
            frames = (int)(m.bytes.size() / bytes_per_frame);
            if (frames <= 0) continue;
            out.resize((size_t)frames * m.channels);
            if (m.out_float) {
                std::memcpy(out.data(), m.bytes.data(), out.size() * sizeof(float));
            } else {
                const int16_t *s16 = reinterpret_cast<const int16_t *>(m.bytes.data());
                for (size_t i = 0; i < out.size(); ++i) out[i] = s16[i] / 32768.0f;
            }
            LONGLONG time = 0;
            if (SUCCEEDED(sample->GetSampleTime(&time))) pts_sec = (double)time / 1e7;
            return AVB_OK;
        }
        if (!m.pump.draining) return AVB_ERROR_AGAIN;
        if (m.pump.drained) return AVB_ERROR_EOF;
        hr = m.pump.wait_drain();
        if (FAILED(hr)) {
            m_last_error = hr_message("Draining the decoder", hr);
            return AVB_ERROR_DECODE_FAILED;
        }
    }
    return AVB_ERROR_AGAIN;
}

avb_result AvbStreamDecoderMediaFoundation::reset() {
    Impl &m = *m_impl;
    if (!m.pump.mft) {
        m.pump.draining = m.pump.drained = false;
        return AVB_OK;
    }
    const HRESULT hr = m.pump.flush();
    if (FAILED(hr)) {
        m_last_error = hr_message("Flushing the decoder", hr);
        return AVB_ERROR_DECODE_FAILED;
    }
    return AVB_OK;
}

#endif // _WIN32
