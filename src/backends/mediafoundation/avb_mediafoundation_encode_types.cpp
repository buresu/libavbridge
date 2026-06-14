#include "avb_mediafoundation_encode_types.hpp"

#ifdef _WIN32

#include "avb_mediafoundation_common.hpp"

#include <mfapi.h>
#include <mferror.h>
#include <mmreg.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

using Microsoft::WRL::ComPtr;

long long mf_encode_seconds_to_hns(double seconds) {
    return static_cast<long long>(std::llround(seconds * 1e7));
}

int mf_encode_mp3_bitrate(int requested) {
    static const int allowed[] = {
        320000, 256000, 224000, 192000, 160000, 128000, 112000,
        96000, 80000, 64000, 56000, 48000, 40000, 32000
    };
    int want = requested > 0 ? requested : 128000;
    int best = allowed[0];
    int best_delta = std::abs(want - best);
    for (int value : allowed) {
        int delta = std::abs(want - value);
        if (delta < best_delta) {
            best = value;
            best_delta = delta;
        }
    }
    return best;
}

unsigned int mf_encode_aac_bytes_per_sec(int bitrate_bps) {
    const unsigned int allowed[] = {12000, 16000, 20000, 24000};
    unsigned int want = bitrate_bps > 0
        ? static_cast<unsigned int>(bitrate_bps / 8)
        : 16000;
    unsigned int best = allowed[0];
    unsigned int best_delta = want > best ? want - best : best - want;
    for (unsigned int value : allowed) {
        unsigned int delta =
            want > value ? want - value : value - want;
        if (delta < best_delta) {
            best = value;
            best_delta = delta;
        }
    }
    return best;
}

HRESULT mf_encode_init_mp3_media_type(
    IMFMediaType *type,
    int sample_rate,
    int channels,
    int bitrate_bps) {
    if (!type) return E_POINTER;

    MPEGLAYER3WAVEFORMAT mp3{};
    mp3.wfx.wFormatTag = WAVE_FORMAT_MPEGLAYER3;
    mp3.wfx.nChannels = static_cast<WORD>(channels);
    mp3.wfx.nSamplesPerSec = static_cast<DWORD>(sample_rate);
    mp3.wfx.nAvgBytesPerSec = static_cast<DWORD>(bitrate_bps / 8);
    mp3.wfx.nBlockAlign = 1;
    mp3.wfx.wBitsPerSample = 0;
    mp3.wfx.cbSize = MPEGLAYER3_WFX_EXTRA_BYTES;
    mp3.wID = MPEGLAYER3_ID_MPEG;
    mp3.fdwFlags = MPEGLAYER3_FLAG_PADDING_ISO;
    mp3.nBlockSize = 1;
    mp3.nFramesPerBlock = 1;
    mp3.nCodecDelay = 0;
    return MFInitMediaTypeFromWaveFormatEx(
        type, &mp3.wfx, sizeof(mp3));
}

HRESULT mf_encode_select_mp3_output_type(
    IMFTransform *encoder,
    IMFMediaType *preferred,
    unsigned int sample_rate,
    unsigned int channels,
    IMFMediaType **selected) {
    if (!encoder || !preferred || !selected) return E_POINTER;
    *selected = nullptr;

    HRESULT hr = encoder->SetOutputType(0, preferred, 0);
    if (SUCCEEDED(hr)) {
        preferred->AddRef();
        *selected = preferred;
        return hr;
    }

    for (DWORD index = 0;; ++index) {
        ComPtr<IMFMediaType> candidate;
        HRESULT type_hr =
            encoder->GetOutputAvailableType(0, index, &candidate);
        if (type_hr == MF_E_NO_MORE_TYPES) break;
        if (FAILED(type_hr) || !candidate) continue;

        UINT32 candidate_rate = 0;
        UINT32 candidate_channels = 0;
        candidate->GetUINT32(
            MF_MT_AUDIO_SAMPLES_PER_SECOND, &candidate_rate);
        candidate->GetUINT32(
            MF_MT_AUDIO_NUM_CHANNELS, &candidate_channels);
        if (candidate_rate != sample_rate ||
            candidate_channels != channels) {
            continue;
        }

        type_hr = encoder->SetOutputType(0, candidate.Get(), 0);
        if (SUCCEEDED(type_hr))
            return candidate.CopyTo(selected);
    }

    return hr;
}

GUID mf_encode_video_subtype(
    avb_video_codec codec,
    std::uint32_t codec_tag) {
    switch (codec) {
        case AVB_VIDEO_CODEC_H264: return MFVideoFormat_H264;
        case AVB_VIDEO_CODEC_HEVC: return MFVideoFormat_HEVC;
        case AVB_VIDEO_CODEC_VP8:  return MFVideoFormat_VP80;
        case AVB_VIDEO_CODEC_VP9:
            return mf_video_subtype_from_fourcc(mf_fourcc("VP90"));
        case AVB_VIDEO_CODEC_AV1:  return MFVideoFormat_AV1;
        default:
            return codec_tag == 0
                ? GUID_NULL
                : mf_video_subtype_from_fourcc(codec_tag);
    }
}

const char *mf_encode_video_codec_name(avb_video_codec codec) {
    switch (codec) {
        case AVB_VIDEO_CODEC_H264: return "H264";
        case AVB_VIDEO_CODEC_HEVC: return "HEVC";
        case AVB_VIDEO_CODEC_VP8:  return "VP8";
        case AVB_VIDEO_CODEC_VP9:  return "VP9";
        case AVB_VIDEO_CODEC_AV1:  return "AV1";
        case AVB_VIDEO_CODEC_HAP:  return "HAP";
        default:                   return "custom";
    }
}

bool mf_encode_is_ivf_codec(avb_video_codec codec) {
    return codec == AVB_VIDEO_CODEC_VP8 ||
           codec == AVB_VIDEO_CODEC_VP9 ||
           codec == AVB_VIDEO_CODEC_AV1;
}

HRESULT mf_encode_select_video_output_type(
    IMFTransform *encoder,
    IMFMediaType *preferred,
    const GUID &subtype,
    unsigned int width,
    unsigned int height,
    unsigned int fps_num,
    unsigned int fps_den) {
    if (!encoder || !preferred) return E_POINTER;
    HRESULT hr = encoder->SetOutputType(0, preferred, 0);
    if (SUCCEEDED(hr)) return hr;

    for (DWORD index = 0;; ++index) {
        ComPtr<IMFMediaType> candidate;
        HRESULT type_hr =
            encoder->GetOutputAvailableType(0, index, &candidate);
        if (type_hr == MF_E_NO_MORE_TYPES) break;
        if (FAILED(type_hr) || !candidate) continue;

        GUID candidate_subtype = GUID_NULL;
        candidate->GetGUID(MF_MT_SUBTYPE, &candidate_subtype);
        if (!IsEqualGUID(candidate_subtype, subtype)) continue;

        candidate->SetUINT32(
            MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(
            candidate.Get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(
            candidate.Get(), MF_MT_FRAME_RATE, fps_num, fps_den);
        MFSetAttributeRatio(
            candidate.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        type_hr = encoder->SetOutputType(0, candidate.Get(), 0);
        if (SUCCEEDED(type_hr)) return type_hr;
    }

    return hr;
}

HRESULT mf_encode_select_video_input_type(
    IMFTransform *encoder,
    IMFMediaType *preferred,
    unsigned int width,
    unsigned int height,
    unsigned int fps_num,
    unsigned int fps_den,
    bool *use_nv12) {
    if (!encoder || !preferred || !use_nv12) return E_POINTER;
    HRESULT hr = encoder->SetInputType(0, preferred, 0);
    if (SUCCEEDED(hr)) {
        *use_nv12 = true;
        return hr;
    }

    for (DWORD index = 0;; ++index) {
        ComPtr<IMFMediaType> candidate;
        HRESULT type_hr =
            encoder->GetInputAvailableType(0, index, &candidate);
        if (type_hr == MF_E_NO_MORE_TYPES) break;
        if (FAILED(type_hr) || !candidate) continue;

        GUID subtype = GUID_NULL;
        candidate->GetGUID(MF_MT_SUBTYPE, &subtype);
        bool nv12 = IsEqualGUID(subtype, MFVideoFormat_NV12);
        bool i420 = IsEqualGUID(subtype, MFVideoFormat_I420);
        if (!nv12 && !i420) continue;

        candidate->SetUINT32(
            MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(
            candidate.Get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(
            candidate.Get(), MF_MT_FRAME_RATE, fps_num, fps_den);
        MFSetAttributeRatio(
            candidate.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        candidate->SetUINT32(MF_MT_DEFAULT_STRIDE, width);
        type_hr = encoder->SetInputType(0, candidate.Get(), 0);
        if (SUCCEEDED(type_hr)) {
            *use_nv12 = nv12;
            return type_hr;
        }
    }

    return hr;
}

HRESULT mf_encode_write_buffer(
    IMFSinkWriter *writer,
    unsigned long stream,
    IMFMediaBuffer *buffer,
    unsigned long length,
    long long time_hns,
    long long duration_hns) {
    if (!writer || !buffer) return E_POINTER;
    buffer->SetCurrentLength(length);

    ComPtr<IMFSample> sample;
    HRESULT hr = MFCreateSample(&sample);
    if (FAILED(hr)) return hr;
    hr = sample->AddBuffer(buffer);
    if (FAILED(hr)) return hr;
    sample->SetSampleTime(time_hns);
    sample->SetSampleDuration(duration_hns);
    return writer->WriteSample(stream, sample.Get());
}

#endif
