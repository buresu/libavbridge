#pragma once

#include "avbridge.h"

#ifdef _WIN32

#include <cstdint>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <vector>

long long mf_encode_seconds_to_hns(double seconds);
int mf_encode_mp3_bitrate(int requested);
unsigned int mf_encode_aac_bytes_per_sec(int bitrate_bps);

HRESULT mf_encode_init_mp3_media_type(
    IMFMediaType *type,
    int sample_rate,
    int channels,
    int bitrate_bps);

HRESULT mf_encode_select_mp3_output_type(
    IMFTransform *encoder,
    IMFMediaType *preferred,
    unsigned int sample_rate,
    unsigned int channels,
    IMFMediaType **selected);

GUID mf_encode_video_subtype(
    avb_video_codec codec,
    std::uint32_t codec_tag);

HRESULT mf_encode_set_mpeg4_video_sample_description(
    IMFMediaType *type,
    std::uint32_t codec_tag,
    unsigned int width,
    unsigned int height);

const char *mf_encode_video_codec_name(avb_video_codec codec);
bool mf_encode_is_ivf_codec(avb_video_codec codec);

HRESULT mf_encode_select_video_output_type(
    IMFTransform *encoder,
    IMFMediaType *preferred,
    const GUID &subtype,
    unsigned int width,
    unsigned int height,
    unsigned int fps_num,
    unsigned int fps_den);

HRESULT mf_encode_select_video_input_type(
    IMFTransform *encoder,
    IMFMediaType *preferred,
    unsigned int width,
    unsigned int height,
    unsigned int fps_num,
    unsigned int fps_den,
    bool *use_nv12);

HRESULT mf_encode_write_buffer(
    IMFSinkWriter *writer,
    unsigned long stream,
    IMFMediaBuffer *buffer,
    unsigned long length,
    long long time_hns,
    long long duration_hns);

struct MfEncodeAudioData {
    const void *data;
    std::size_t size;
};

MfEncodeAudioData mf_encode_pack_audio_f32(
    const float *samples,
    int frames,
    int channels,
    bool keep_float,
    std::vector<std::int16_t> &output);

std::size_t mf_encode_pack_video_frame(
    const avb_video_frame &frame,
    int width,
    int height,
    avb_pixel_format input_format,
    bool convert_i420_to_nv12,
    std::vector<unsigned char> &output);

#endif
