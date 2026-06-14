#include "avb_mediafoundation_ivf.hpp"

#ifdef _WIN32

#include "avb_mediafoundation_common.hpp"

#include <mfapi.h>
#include <mferror.h>
#include <wrl/client.h>

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {

avb_video_codec codec_from_fourcc(uint32_t fourcc) {
    if (fourcc == mf_fourcc("VP80")) return AVB_VIDEO_CODEC_VP8;
    if (fourcc == mf_fourcc("VP90")) return AVB_VIDEO_CODEC_VP9;
    if (fourcc == mf_fourcc("AV01")) return AVB_VIDEO_CODEC_AV1;
    return AVB_VIDEO_CODEC_AUTO;
}

uint32_t fourcc_from_codec(avb_video_codec codec) {
    switch (codec) {
        case AVB_VIDEO_CODEC_VP8: return mf_fourcc("VP80");
        case AVB_VIDEO_CODEC_VP9: return mf_fourcc("VP90");
        case AVB_VIDEO_CODEC_AV1: return mf_fourcc("AV01");
        default: return 0;
    }
}

} // namespace

GUID mf_ivf_codec_subtype(avb_video_codec codec) {
    switch (codec) {
        case AVB_VIDEO_CODEC_VP8:
            return MFVideoFormat_VP80;
        case AVB_VIDEO_CODEC_VP9:
            return mf_video_subtype_from_fourcc(mf_fourcc("VP90"));
        case AVB_VIDEO_CODEC_AV1:
            return MFVideoFormat_AV1;
        default:
            return GUID_NULL;
    }
}

MfIvfReadResult mf_ivf_read_header(FILE *file, MfIvfHeader &out) {
    if (!file) return MfIvfReadResult::invalid;

    unsigned char bytes[32] = {};
    if (std::fread(bytes, 1, sizeof(bytes), file) != sizeof(bytes) ||
        std::memcmp(bytes, "DKIF", 4) != 0 ||
        mf_read_le16(bytes + 6) < sizeof(bytes)) {
        return MfIvfReadResult::invalid;
    }

    out.codec = codec_from_fourcc(mf_read_le32(bytes + 8));
    out.width = static_cast<int>(mf_read_le16(bytes + 12));
    out.height = static_cast<int>(mf_read_le16(bytes + 14));
    out.rate = mf_read_le32(bytes + 16);
    out.scale = mf_read_le32(bytes + 20);
    out.frame_count = mf_read_le32(bytes + 24);
    out.data_offset = static_cast<long>(mf_read_le16(bytes + 6));

    if (out.codec == AVB_VIDEO_CODEC_AUTO)
        return MfIvfReadResult::unsupported;
    if (out.width <= 0 || out.height <= 0 ||
        out.rate == 0 || out.scale == 0) {
        return MfIvfReadResult::invalid;
    }
    if (out.data_offset > static_cast<long>(sizeof(bytes)) &&
        std::fseek(file, out.data_offset, SEEK_SET) != 0) {
        return MfIvfReadResult::invalid;
    }
    return MfIvfReadResult::ok;
}

HRESULT mf_ivf_select_decoder_output(
    IMFTransform *decoder,
    int *width,
    int *height,
    uint32_t rate,
    uint32_t scale,
    DWORD *output_size,
    DWORD *output_flags) {
    if (!decoder || !width || !height ||
        !output_size || !output_flags) {
        return E_POINTER;
    }

    HRESULT result = MF_E_INVALIDMEDIATYPE;
    for (DWORD index = 0;; ++index) {
        ComPtr<IMFMediaType> candidate;
        HRESULT hr =
            decoder->GetOutputAvailableType(0, index, &candidate);
        if (hr == MF_E_NO_MORE_TYPES) break;
        if (FAILED(hr) || !candidate) continue;

        GUID subtype = GUID_NULL;
        candidate->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (!IsEqualGUID(subtype, MFVideoFormat_NV12)) continue;

        if (*width > 0 && *height > 0) {
            MFSetAttributeSize(
                candidate.Get(), MF_MT_FRAME_SIZE,
                static_cast<UINT32>(*width),
                static_cast<UINT32>(*height));
        }
        if (rate > 0 && scale > 0) {
            MFSetAttributeRatio(
                candidate.Get(), MF_MT_FRAME_RATE, rate, scale);
        }
        result = decoder->SetOutputType(0, candidate.Get(), 0);
        if (FAILED(result)) continue;

        UINT32 selected_width = 0;
        UINT32 selected_height = 0;
        if (SUCCEEDED(MFGetAttributeSize(
                candidate.Get(), MF_MT_FRAME_SIZE,
                &selected_width, &selected_height)) &&
            selected_width > 0 && selected_height > 0) {
            *width = static_cast<int>(selected_width);
            *height = static_cast<int>(selected_height);
        }
        break;
    }
    if (FAILED(result)) return result;

    MFT_OUTPUT_STREAM_INFO info{};
    if (SUCCEEDED(decoder->GetOutputStreamInfo(0, &info))) {
        *output_size = info.cbSize;
        *output_flags = info.dwFlags;
    }
    if (*output_size == 0 && *width > 0 && *height > 0) {
        *output_size = static_cast<DWORD>(
            static_cast<std::size_t>(*width) * *height * 3 / 2);
    }
    return S_OK;
}

HRESULT mf_ivf_configure_decoder_types(
    IMFTransform *decoder,
    const MfIvfHeader &header,
    DWORD *output_size,
    DWORD *output_flags) {
    if (!decoder || !output_size || !output_flags) return E_POINTER;

    const GUID input_subtype = mf_ivf_codec_subtype(header.codec);
    if (IsEqualGUID(input_subtype, GUID_NULL))
        return MF_E_INVALIDMEDIATYPE;

    ComPtr<IMFMediaType> input_type;
    HRESULT hr = MFCreateMediaType(&input_type);
    if (FAILED(hr)) return hr;
    input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input_type->SetGUID(MF_MT_SUBTYPE, input_subtype);
    input_type->SetUINT32(
        MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(
        input_type.Get(), MF_MT_FRAME_SIZE,
        static_cast<UINT32>(header.width),
        static_cast<UINT32>(header.height));
    MFSetAttributeRatio(
        input_type.Get(), MF_MT_FRAME_RATE,
        header.rate, header.scale);
    MFSetAttributeRatio(
        input_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = decoder->SetInputType(0, input_type.Get(), 0);
    if (FAILED(hr)) return hr;

    ComPtr<IMFMediaType> output_type;
    hr = MFCreateMediaType(&output_type);
    if (FAILED(hr)) return hr;
    output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    output_type->SetUINT32(
        MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(
        output_type.Get(), MF_MT_FRAME_SIZE,
        static_cast<UINT32>(header.width),
        static_cast<UINT32>(header.height));
    MFSetAttributeRatio(
        output_type.Get(), MF_MT_FRAME_RATE,
        header.rate, header.scale);
    MFSetAttributeRatio(
        output_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = decoder->SetOutputType(0, output_type.Get(), 0);
    if (FAILED(hr)) {
        int width = header.width;
        int height = header.height;
        return mf_ivf_select_decoder_output(
            decoder, &width, &height, header.rate, header.scale,
            output_size, output_flags);
    }

    MFT_OUTPUT_STREAM_INFO info{};
    if (SUCCEEDED(decoder->GetOutputStreamInfo(0, &info))) {
        *output_size = info.cbSize;
        *output_flags = info.dwFlags;
    }
    if (*output_size == 0) {
        *output_size = static_cast<DWORD>(
            static_cast<std::size_t>(header.width) *
            header.height * 3 / 2);
    }
    return S_OK;
}

MfIvfReadResult mf_ivf_read_frame(
    FILE *file,
    std::vector<unsigned char> &packet,
    uint64_t &timestamp) {
    if (!file) return MfIvfReadResult::invalid;

    unsigned char header[12] = {};
    size_t read = std::fread(header, 1, sizeof(header), file);
    if (read != sizeof(header)) return MfIvfReadResult::eof;

    uint32_t packet_size = mf_read_le32(header);
    timestamp = mf_read_le64(header + 4);
    if (packet_size == 0 || packet_size > 256u * 1024u * 1024u)
        return MfIvfReadResult::invalid;

    packet.resize(packet_size);
    if (std::fread(packet.data(), 1, packet_size, file) != packet_size)
        return MfIvfReadResult::invalid;
    return MfIvfReadResult::ok;
}

bool mf_ivf_write_header(FILE *file, const MfIvfHeader &header) {
    uint32_t fourcc = fourcc_from_codec(header.codec);
    if (!file || !fourcc || header.width <= 0 || header.height <= 0)
        return false;

    unsigned char bytes[32] = {};
    std::memcpy(bytes, "DKIF", 4);
    mf_write_le16(bytes + 4, 0);
    mf_write_le16(bytes + 6, sizeof(bytes));
    mf_write_le32(bytes + 8, fourcc);
    mf_write_le16(bytes + 12, static_cast<uint16_t>(header.width));
    mf_write_le16(bytes + 14, static_cast<uint16_t>(header.height));
    mf_write_le32(bytes + 16, header.rate ? header.rate : 30);
    mf_write_le32(bytes + 20, header.scale ? header.scale : 1);
    mf_write_le32(bytes + 24, header.frame_count);
    return std::fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
}

bool mf_ivf_write_frame(
    FILE *file,
    const unsigned char *data,
    uint32_t size,
    uint64_t timestamp) {
    if (!file || !data || size == 0) return false;

    unsigned char header[12] = {};
    mf_write_le32(header, size);
    mf_write_le64(header + 4, timestamp);
    return std::fwrite(header, 1, sizeof(header), file) == sizeof(header) &&
           std::fwrite(data, 1, size, file) == size;
}

#endif
