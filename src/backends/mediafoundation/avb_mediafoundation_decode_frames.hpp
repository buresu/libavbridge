#pragma once

#include "avbridge.h"

#ifdef _WIN32

#include <mfidl.h>
#include <vector>

avb_result mf_decode_copy_cpu_frame(
    IMFSample *sample,
    int width,
    int height,
    int source_stride,
    bool bottom_up,
    avb_pixel_format output_format,
    double pts_sec,
    std::vector<unsigned char> &storage,
    avb_video_frame &output);

#endif
