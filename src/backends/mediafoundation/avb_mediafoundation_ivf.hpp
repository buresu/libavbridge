#pragma once

#ifdef _WIN32

#include "avbridge.h"

#include <cstdint>
#include <cstdio>
#include <mfidl.h>
#include <vector>

struct MfIvfHeader {
    avb_video_codec codec = AVB_VIDEO_CODEC_AUTO;
    int width = 0;
    int height = 0;
    uint32_t rate = 0;
    uint32_t scale = 0;
    uint32_t frame_count = 0;
    long data_offset = 32;
};

enum class MfIvfReadResult {
    ok,
    eof,
    unsupported,
    invalid,
};

MfIvfReadResult mf_ivf_read_header(FILE *file, MfIvfHeader &out);
MfIvfReadResult mf_ivf_read_frame(
    FILE *file,
    std::vector<unsigned char> &packet,
    uint64_t &timestamp);

GUID mf_ivf_codec_subtype(avb_video_codec codec);

HRESULT mf_ivf_configure_decoder_types(
    IMFTransform *decoder,
    const MfIvfHeader &header,
    DWORD *output_size,
    DWORD *output_flags);

HRESULT mf_ivf_select_decoder_output(
    IMFTransform *decoder,
    int *width,
    int *height,
    uint32_t rate,
    uint32_t scale,
    DWORD *output_size,
    DWORD *output_flags);

HRESULT mf_ivf_configure_encoder_types(
    IMFTransform *encoder,
    const MfIvfHeader &header,
    int bitrate,
    bool *input_nv12,
    DWORD *output_size,
    DWORD *output_flags);

bool mf_ivf_write_header(FILE *file, const MfIvfHeader &header);
bool mf_ivf_write_frame(
    FILE *file,
    const unsigned char *data,
    uint32_t size,
    uint64_t timestamp);

#endif
