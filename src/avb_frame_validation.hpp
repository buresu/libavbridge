#pragma once

#include "avbridge.h"

#include <cstdint>

namespace avb::detail {

// Validate an encoder input frame against the encoder's video parameters and
// normalize it in place (packed-format convenience input gets its plane 0,
// data/stride alias plane 0). Returns a static diagnostic, or nullptr when
// the frame is acceptable. Native handles keep their backend-specific checks.
inline const char *prepare_video_frame(const avb_video_encode_params &video,
                                       avb_video_frame &frame) {
    if (frame.width != video.width || frame.height != video.height)
        return "Video frame dimensions do not match encoder options.";
    if (frame.memory_type != video.input_memory ||
        (frame.memory_type == AVB_VIDEO_MEMORY_EXTERNAL &&
         frame.external_type != video.input_external_type))
        return "Video frame memory representation does not match encoder options.";
    if (frame.memory_type != AVB_VIDEO_MEMORY_CPU) return nullptr;

    const auto format = video.input_format == AVB_PIXEL_FORMAT_UNKNOWN
        ? AVB_PIXEL_FORMAT_BGRA8 : video.input_format;
    if (frame.format != format)
        return "Frame pixel format does not match configured input_format.";

    const int64_t width = frame.width;
    int planes = 1;
    int64_t row_bytes[AVB_MAX_PLANES] = {};
    int64_t rows = frame.height;
    switch (format) {
        case AVB_PIXEL_FORMAT_RGBA8:
        case AVB_PIXEL_FORMAT_BGRA8:
            row_bytes[0] = width * 4;
            // Packed-format convenience input uses only data/stride.
            if (frame.plane_count == 0) {
                frame.plane_count = 1;
                frame.plane_data[0] = frame.data;
                frame.plane_stride[0] = frame.stride;
            }
            break;
        case AVB_PIXEL_FORMAT_NV12:
            planes = 2;
            row_bytes[0] = width;
            row_bytes[1] = ((width + 1) / 2) * 2;
            break;
        case AVB_PIXEL_FORMAT_I420:
            planes = 3;
            row_bytes[0] = width;
            row_bytes[1] = row_bytes[2] = (width + 1) / 2;
            break;
        default:
            rows = (rows + 3) / 4;
            row_bytes[0] = ((width + 3) / 4) *
                ((format == AVB_PIXEL_FORMAT_BC1_RGBA ||
                  format == AVB_PIXEL_FORMAT_BC4_R) ? 8 : 16);
            if (format == AVB_PIXEL_FORMAT_BC3_YCOCG_BC4_A) {
                planes = 2;
                row_bytes[1] = ((width + 3) / 4) * 8;
            }
            break;
    }
    if (frame.plane_count != planes)
        return "Video frame has an incorrect plane count.";
    for (int p = 0; p < planes; ++p) {
        if (!frame.plane_data[p] || frame.plane_stride[p] < row_bytes[p])
            return "Video frame has a missing plane or an insufficient positive stride.";
    }
    if (planes == 1 && frame.data_size > 0 &&
        (!frame.data || frame.data == frame.plane_data[0]) &&
        frame.data_size < (rows - 1) * frame.plane_stride[0] + row_bytes[0])
        return "Video frame backing buffer is too small.";
    frame.data = frame.plane_data[0];
    frame.stride = frame.plane_stride[0];
    return nullptr;
}

} // namespace avb::detail
