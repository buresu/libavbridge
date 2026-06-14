#pragma once

#include "avbridge.h"

#include <cstddef>

// Tightly-packed CPU plane layout shared by the decoder/encoder backends.
//
// Backends that hand out (or repack into) a contiguous CPU buffer all need the
// same per-plane stride/rows/offset arithmetic for the packed/planar formats:
//   RGBA8 / BGRA8 : 1 plane,  stride = w*4
//   NV12          : 2 planes, Y (w) + interleaved CbCr (w) at half height
//   I420          : 3 planes, Y (w) + Cb (w/2) + Cr (w/2) at half height
//
// `align` rounds each plane stride up to a multiple of that many bytes:
//   1 -> exact (FFmpeg decoder writes its own buffer with stride == width)
//   4 -> GStreamer's GST_ROUND_UP_4 (matches gstreamer's raw-video strides)
struct AvbPlaneLayout {
    int    plane_count = 1;
    int    stride[AVB_MAX_PLANES] = {0, 0, 0};
    int    rows[AVB_MAX_PLANES]   = {0, 0, 0};
    size_t offset[AVB_MAX_PLANES] = {0, 0, 0};
    size_t total = 0;
};

inline AvbPlaneLayout avb_plane_layout(avb_pixel_format fmt, int w, int h, int align) {
    auto aligned = [align](int base) {
        if (align <= 1) return base;
        return (base + (align - 1)) & ~(align - 1);
    };

    AvbPlaneLayout l;
    switch (fmt) {
        case AVB_PIXEL_FORMAT_NV12:
            l.plane_count = 2;
            l.stride[0] = aligned(w);     l.rows[0] = h;
            l.stride[1] = aligned(w);     l.rows[1] = h / 2;
            break;
        case AVB_PIXEL_FORMAT_I420:
            l.plane_count = 3;
            l.stride[0] = aligned(w);     l.rows[0] = h;
            l.stride[1] = aligned(w / 2); l.rows[1] = h / 2;
            l.stride[2] = aligned(w / 2); l.rows[2] = h / 2;
            break;
        default: // RGBA8 / BGRA8
            l.plane_count = 1;
            l.stride[0] = aligned(w * 4); l.rows[0] = h;
            break;
    }
    for (int p = 0; p < l.plane_count; ++p) {
        l.offset[p] = l.total;
        l.total += (size_t)l.stride[p] * l.rows[p];
    }
    return l;
}
