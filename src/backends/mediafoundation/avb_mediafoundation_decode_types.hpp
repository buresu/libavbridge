#pragma once

#include "avbridge.h"

#ifdef _WIN32

#include <mfidl.h>
#include <mfreadwrite.h>
#include <string>

void mf_decode_find_stream_indices(
    IMFSourceReader *reader,
    int *audio_idx,
    int *video_idx,
    int *audio_count);

std::string mf_decode_native_codec_name(
    IMFSourceReader *reader,
    unsigned long stream);

avb_result mf_decode_open_custom_video(
    IMFSourceReader *reader,
    unsigned long stream_idx,
    const avb_decode_options &options,
    const avb_video_decoder_plugin **out_plugin,
    void **out_ctx,
    std::string &out_codec_name,
    int *out_width,
    int *out_height,
    double *out_frame_rate);

#endif
