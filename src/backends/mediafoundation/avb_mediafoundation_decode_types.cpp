#include "avb_mediafoundation_decode_types.hpp"

#ifdef _WIN32

#include "avb_video_plugins.hpp"

#include <mfapi.h>
#include <mferror.h>
#include <wrl/client.h>

#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

std::string subtype_name(const GUID &subtype) {
    static const struct {
        const GUID *guid;
        const char *name;
    } mappings[] = {
        {&MFVideoFormat_H264, "h264"},
        {&MFVideoFormat_HEVC, "hevc"},
        {&MFVideoFormat_MPEG2, "mpeg2video"},
        {&MFVideoFormat_MP4V, "mpeg4"},
        {&MFVideoFormat_MJPG, "mjpeg"},
        {&MFVideoFormat_WMV3, "wmv3"},
        {&MFAudioFormat_AAC, "aac"},
        {&MFAudioFormat_MP3, "mp3"},
        {&MFAudioFormat_Dolby_AC3, "ac3"},
        {&MFAudioFormat_PCM, "pcm"},
        {&MFAudioFormat_Float, "pcm_f32"},
    };
    for (const auto &mapping : mappings) {
        if (IsEqualGUID(subtype, *mapping.guid)) return mapping.name;
    }

    static const GUID h264_es = {
        0x3f40f4f0, 0x5622, 0x4ff8,
        {0xb6, 0xd8, 0xa1, 0x7a, 0x58, 0x4b, 0xee, 0x5e}};
    if (IsEqualGUID(subtype, h264_es)) return "h264";

    static const GUID vorbis = {
        0x8d2fd10b, 0x5841, 0x4a6b,
        {0x89, 0x05, 0x58, 0x8f, 0xec, 0x1a, 0xde, 0xd9}};
    if (IsEqualGUID(subtype, vorbis)) return "vorbis";

    if (subtype.Data2 == 0x0000 && subtype.Data3 == 0x0010 &&
        subtype.Data4[0] == 0x80 && subtype.Data4[1] == 0x00 &&
        subtype.Data4[2] == 0x00 && subtype.Data4[3] == 0xaa &&
        subtype.Data4[4] == 0x00 && subtype.Data4[5] == 0x38 &&
        subtype.Data4[6] == 0x9b && subtype.Data4[7] == 0x71) {
        if (subtype.Data1 == 0x704f) return "opus";
        if (subtype.Data1 == 0xf1ac) return "flac";
    }

    unsigned long fourcc = subtype.Data1;
    char name[5] = {
        static_cast<char>(fourcc & 0xff),
        static_cast<char>((fourcc >> 8) & 0xff),
        static_cast<char>((fourcc >> 16) & 0xff),
        static_cast<char>((fourcc >> 24) & 0xff),
        0};
    if (std::strcmp(name, "H265") == 0 ||
        std::strcmp(name, "HEVS") == 0) {
        return "hevc";
    }
    if (std::strcmp(name, "VP80") == 0) return "vp8";
    if (std::strcmp(name, "VP90") == 0) return "vp9";
    if (std::strcmp(name, "AV01") == 0) return "av1";
    for (int i = 0; i < 4; ++i) {
        if (name[i] < 0x20 || name[i] > 0x7e) name[i] = '?';
    }
    return name;
}

bool get_blob(
    IMFMediaType *type,
    REFGUID key,
    std::vector<unsigned char> &output) {
    UINT32 size = 0;
    if (!type || FAILED(type->GetBlobSize(key, &size)) || size == 0)
        return false;

    output.resize(size);
    UINT32 written = 0;
    if (FAILED(type->GetBlob(key, output.data(), size, &written))) {
        output.clear();
        return false;
    }
    output.resize(written);
    return true;
}

} // namespace

void mf_decode_find_stream_indices(
    IMFSourceReader *reader,
    int *audio_idx,
    int *video_idx,
    int *audio_count) {
    *audio_idx = -1;
    *video_idx = -1;
    *audio_count = 0;
    for (DWORD index = 0;; ++index) {
        ComPtr<IMFMediaType> type;
        if (FAILED(reader->GetNativeMediaType(index, 0, &type))) break;

        GUID major = GUID_NULL;
        type->GetGUID(MF_MT_MAJOR_TYPE, &major);
        if (IsEqualGUID(major, MFMediaType_Audio)) {
            if (*audio_idx < 0) *audio_idx = static_cast<int>(index);
            ++*audio_count;
        }
        if (*video_idx < 0 && IsEqualGUID(major, MFMediaType_Video))
            *video_idx = static_cast<int>(index);
    }
}

std::string mf_decode_native_codec_name(
    IMFSourceReader *reader,
    unsigned long stream) {
    std::string first;
    for (DWORD index = 0;; ++index) {
        ComPtr<IMFMediaType> native;
        if (FAILED(reader->GetNativeMediaType(stream, index, &native)) ||
            !native) {
            break;
        }

        GUID subtype = GUID_NULL;
        if (FAILED(native->GetGUID(MF_MT_SUBTYPE, &subtype))) continue;
        std::string name = subtype_name(subtype);
        if (first.empty()) first = name;
        if (name != "pcm" && name != "pcm_f32") return name;
    }
    return first;
}

HRESULT mf_decode_configure_audio(
    IMFSourceReader *reader,
    unsigned long stream,
    const avb_decode_options &options,
    MfDecodeAudioFormat *format) {
    if (!reader || !format) return E_POINTER;
    *format = {};

    ComPtr<IMFMediaType> requested;
    HRESULT hr = MFCreateMediaType(&requested);
    if (FAILED(hr)) return hr;
    requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    requested->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    if (options.audio_sample_rate > 0) {
        requested->SetUINT32(
            MF_MT_AUDIO_SAMPLES_PER_SECOND,
            static_cast<UINT32>(options.audio_sample_rate));
    }
    if (options.audio_channels > 0) {
        requested->SetUINT32(
            MF_MT_AUDIO_NUM_CHANNELS,
            static_cast<UINT32>(options.audio_channels));
    }

    hr = reader->SetCurrentMediaType(stream, nullptr, requested.Get());
    if (FAILED(hr)) return hr;
    reader->SetStreamSelection(stream, TRUE);

    ComPtr<IMFMediaType> current;
    reader->GetCurrentMediaType(stream, &current);
    if (current) {
        UINT32 sample_rate = 0;
        UINT32 channels = 0;
        current->GetUINT32(
            MF_MT_AUDIO_SAMPLES_PER_SECOND, &sample_rate);
        current->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
        format->sample_rate = static_cast<int>(sample_rate);
        format->channels = static_cast<int>(channels);
    }
    return S_OK;
}

HRESULT mf_decode_refresh_video_format(
    IMFSourceReader *reader,
    unsigned long stream,
    bool native_output,
    MfDecodeVideoFormat *format) {
    if (!reader || !format) return E_POINTER;

    ComPtr<IMFMediaType> current;
    HRESULT hr = reader->GetCurrentMediaType(stream, &current);
    if (FAILED(hr) || !current) return FAILED(hr) ? hr : E_FAIL;

    GUID subtype = GUID_NULL;
    hr = current->GetGUID(MF_MT_SUBTYPE, &subtype);
    if (FAILED(hr)) return hr;
    if (native_output && !IsEqualGUID(subtype, MFVideoFormat_NV12))
        return MF_E_INVALIDMEDIATYPE;

    UINT32 width = 0;
    UINT32 height = 0;
    hr = MFGetAttributeSize(
        current.Get(), MF_MT_FRAME_SIZE, &width, &height);
    if (FAILED(hr) || width == 0 || height == 0)
        return FAILED(hr) ? hr : E_FAIL;
    format->width = static_cast<int>(width);
    format->height = static_cast<int>(height);

    UINT32 numerator = 0;
    UINT32 denominator = 1;
    if (SUCCEEDED(MFGetAttributeRatio(
            current.Get(), MF_MT_FRAME_RATE,
            &numerator, &denominator)) &&
        denominator != 0) {
        format->frame_rate =
            static_cast<double>(numerator) / denominator;
    }

    UINT32 stride_raw = 0;
    if (SUCCEEDED(
            current->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride_raw))) {
        INT32 stride = static_cast<INT32>(stride_raw);
        format->bottom_up = stride < 0;
        format->stride = stride < 0 ? -stride : stride;
    } else {
        format->bottom_up = false;
        format->stride = IsEqualGUID(subtype, MFVideoFormat_ARGB32)
            ? static_cast<int>(width) * 4
            : static_cast<int>(width);
    }
    return S_OK;
}

HRESULT mf_decode_configure_video(
    IMFSourceReader *reader,
    unsigned long stream,
    avb_pixel_format requested_format,
    bool native_output,
    MfDecodeVideoFormat *format) {
    if (!reader || !format) return E_POINTER;
    *format = {};

    GUID subtype = MFVideoFormat_ARGB32;
    if (requested_format == AVB_PIXEL_FORMAT_NV12 ||
        (native_output &&
         requested_format == AVB_PIXEL_FORMAT_UNKNOWN)) {
        format->pixel_format = AVB_PIXEL_FORMAT_NV12;
        subtype = MFVideoFormat_NV12;
    } else if (requested_format == AVB_PIXEL_FORMAT_I420) {
        format->pixel_format = AVB_PIXEL_FORMAT_I420;
        subtype = MFVideoFormat_I420;
    } else {
        format->pixel_format =
            requested_format == AVB_PIXEL_FORMAT_RGBA8
            ? AVB_PIXEL_FORMAT_RGBA8
            : AVB_PIXEL_FORMAT_BGRA8;
    }

    ComPtr<IMFMediaType> requested;
    HRESULT hr = MFCreateMediaType(&requested);
    if (FAILED(hr)) return hr;
    requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    requested->SetGUID(MF_MT_SUBTYPE, subtype);

    hr = reader->SetCurrentMediaType(stream, nullptr, requested.Get());
    if (FAILED(hr)) return hr;
    reader->SetStreamSelection(stream, TRUE);
    return mf_decode_refresh_video_format(
        reader, stream, native_output, format);
}

avb_result mf_decode_open_custom_video(
    IMFSourceReader *reader,
    unsigned long stream_idx,
    const avb_decode_options &options,
    const avb_video_decoder_plugin **out_plugin,
    void **out_ctx,
    std::string &out_codec_name,
    int *out_width,
    int *out_height,
    double *out_frame_rate) {
    if (!options.enable_custom_video_decoders)
        return AVB_ERROR_STREAM_NOT_FOUND;

    ComPtr<IMFMediaType> native;
    if (FAILED(reader->GetNativeMediaType(stream_idx, 0, &native)) || !native)
        return AVB_ERROR_STREAM_NOT_FOUND;

    GUID subtype = GUID_NULL;
    native->GetGUID(MF_MT_SUBTYPE, &subtype);

    UINT32 width = 0;
    UINT32 height = 0;
    MFGetAttributeSize(
        native.Get(), MF_MT_FRAME_SIZE, &width, &height);
    UINT32 fps_num = 0;
    UINT32 fps_den = 1;
    MFGetAttributeRatio(
        native.Get(), MF_MT_FRAME_RATE, &fps_num, &fps_den);

    std::vector<unsigned char> extradata;
    get_blob(native.Get(), MF_MT_MPEG_SEQUENCE_HEADER, extradata);

    avb_video_stream_info stream{};
    stream.stream_index = static_cast<int>(stream_idx);
    stream.width = static_cast<int>(width);
    stream.height = static_cast<int>(height);
    stream.frame_rate =
        fps_den != 0 ? static_cast<double>(fps_num) / fps_den : 0.0;
    stream.codec_tag = subtype.Data1;
    stream.extradata = extradata.empty() ? nullptr : extradata.data();
    stream.extradata_size = static_cast<int>(extradata.size());
    stream.time_base_num = 1;
    stream.time_base_den = 10000000;

    out_codec_name = subtype_name(subtype);
    stream.codec_name =
        out_codec_name.empty() ? nullptr : out_codec_name.c_str();

    const avb_video_decoder_plugin *plugin =
        avb_find_video_decoder_plugin(stream, options);
    if (!plugin) return AVB_ERROR_STREAM_NOT_FOUND;

    void *context = nullptr;
    avb_result result = plugin->open(&context, &stream, &options);
    if (result != AVB_OK) return result;

    HRESULT hr =
        reader->SetCurrentMediaType(stream_idx, nullptr, native.Get());
    if (FAILED(hr)) {
        if (plugin->close && context) plugin->close(context);
        return AVB_ERROR_OPEN_FAILED;
    }

    *out_plugin = plugin;
    *out_ctx = context;
    if (out_width) *out_width = static_cast<int>(width);
    if (out_height) *out_height = static_cast<int>(height);
    if (out_frame_rate) *out_frame_rate = stream.frame_rate;
    return AVB_OK;
}

#endif
