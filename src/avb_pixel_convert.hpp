#pragma once

#include "avbridge.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

// CPU colour conversion between the packed RGB and 4:2:0 YUV layouts the
// elementary-stream backends exchange with codecs that take or give only one
// of them (Media Foundation's MFTs, VideoToolbox). Limited-range YUV, BT.709
// for HD sizes and BT.601 otherwise -- the same guess FFmpeg's decoder makes
// when a stream declares nothing.

namespace avb::detail {

inline bool use_bt709(int width, int height) {
    return width > 1024 || height >= 600;
}

inline int chroma_size(int luma) { return (luma + 1) / 2; }

// Tightly packed 4:2:0 output: Y (w*h), then NV12 CbCr (2*cw*ch) or I420
// Cb, Cr (cw*ch each).
inline size_t yuv420_size(int w, int h) {
    return (size_t)w * h + 2 * (size_t)chroma_size(w) * chroma_size(h);
}

// `src` is a validated CPU frame in BGRA8, RGBA8, NV12 or I420.
inline void convert_to_yuv420(const avb_video_frame &src, bool nv12_out, bool bt709,
                              std::vector<uint8_t> &out) {
    const int w = src.width, h = src.height;
    const int cw = chroma_size(w), ch = chroma_size(h);
    out.resize(yuv420_size(w, h));
    uint8_t *y_out = out.data();
    uint8_t *c_out = out.data() + (size_t)w * h;

    if (src.format == AVB_PIXEL_FORMAT_NV12 || src.format == AVB_PIXEL_FORMAT_I420) {
        for (int y = 0; y < h; ++y)
            std::memcpy(y_out + (size_t)y * w, src.plane_data[0] + (size_t)y * src.plane_stride[0], w);
        const bool nv12_in = src.format == AVB_PIXEL_FORMAT_NV12;
        for (int y = 0; y < ch; ++y) {
            for (int x = 0; x < cw; ++x) {
                uint8_t u, v;
                if (nv12_in) {
                    const uint8_t *row = src.plane_data[1] + (size_t)y * src.plane_stride[1];
                    u = row[x * 2];
                    v = row[x * 2 + 1];
                } else {
                    u = src.plane_data[1][(size_t)y * src.plane_stride[1] + x];
                    v = src.plane_data[2][(size_t)y * src.plane_stride[2] + x];
                }
                if (nv12_out) {
                    c_out[(size_t)y * cw * 2 + x * 2] = u;
                    c_out[(size_t)y * cw * 2 + x * 2 + 1] = v;
                } else {
                    c_out[(size_t)y * cw + x] = u;
                    c_out[(size_t)cw * ch + (size_t)y * cw + x] = v;
                }
            }
        }
        return;
    }

    const bool rgba = src.format == AVB_PIXEL_FORMAT_RGBA8;
    const int ri = rgba ? 0 : 2, bi = rgba ? 2 : 0;
    // Fixed-point (x256) limited-range coefficients.
    const int yr = bt709 ? 47 : 66, yg = bt709 ? 157 : 129, yb = bt709 ? 16 : 25;
    const int ur = bt709 ? -26 : -38, ug = bt709 ? -87 : -74, ub = 112;
    const int vr = 112, vg = bt709 ? -102 : -94, vb = bt709 ? -10 : -18;
    for (int y = 0; y < h; ++y) {
        const uint8_t *row = src.plane_data[0] + (size_t)y * src.plane_stride[0];
        uint8_t *dst = y_out + (size_t)y * w;
        for (int x = 0; x < w; ++x) {
            const int r = row[x * 4 + ri], g = row[x * 4 + 1], b = row[x * 4 + bi];
            dst[x] = (uint8_t)std::clamp(16 + ((yr * r + yg * g + yb * b + 128) >> 8), 0, 255);
        }
    }
    for (int y = 0; y < ch; ++y) {
        const int y0 = y * 2, y1 = std::min(y0 + 1, h - 1);
        const uint8_t *row0 = src.plane_data[0] + (size_t)y0 * src.plane_stride[0];
        const uint8_t *row1 = src.plane_data[0] + (size_t)y1 * src.plane_stride[0];
        for (int x = 0; x < cw; ++x) {
            const int x0 = x * 2, x1 = std::min(x0 + 1, w - 1);
            const int r = (row0[x0 * 4 + ri] + row0[x1 * 4 + ri] + row1[x0 * 4 + ri] + row1[x1 * 4 + ri] + 2) >> 2;
            const int g = (row0[x0 * 4 + 1] + row0[x1 * 4 + 1] + row1[x0 * 4 + 1] + row1[x1 * 4 + 1] + 2) >> 2;
            const int b = (row0[x0 * 4 + bi] + row0[x1 * 4 + bi] + row1[x0 * 4 + bi] + row1[x1 * 4 + bi] + 2) >> 2;
            const uint8_t u = (uint8_t)std::clamp(128 + ((ur * r + ug * g + ub * b + 128) >> 8), 0, 255);
            const uint8_t v = (uint8_t)std::clamp(128 + ((vr * r + vg * g + vb * b + 128) >> 8), 0, 255);
            if (nv12_out) {
                c_out[(size_t)y * cw * 2 + x * 2] = u;
                c_out[(size_t)y * cw * 2 + x * 2 + 1] = v;
            } else {
                c_out[(size_t)y * cw + x] = u;
                c_out[(size_t)cw * ch + (size_t)y * cw + x] = v;
            }
        }
    }
}

// 4:2:0 source planes. For NV12 `u` points at the interleaved CbCr plane and
// `v` is unused.
struct Yuv420Planes {
    const uint8_t *y = nullptr;
    int y_stride = 0;
    const uint8_t *u = nullptr;
    int u_stride = 0;
    const uint8_t *v = nullptr;
    int v_stride = 0;
    bool nv12 = true;
};

inline void init_cpu_frame(avb_video_frame &out, int w, int h, avb_pixel_format format,
                           double pts_sec) {
    out = {};
    out.width = w;
    out.height = h;
    out.format = format;
    out.pts_sec = pts_sec;
    out.memory_type = AVB_VIDEO_MEMORY_CPU;
    out.hardware_device = AVB_HW_DEVICE_AUTO;
    for (int p = 0; p < AVB_MAX_PLANES; ++p) out.dmabuf_fd[p] = -1;
}

// Write a CPU frame in `format` (BGRA8, RGBA8, NV12 or I420) into `storage`.
inline void yuv420_to_frame(const Yuv420Planes &src, int w, int h, avb_pixel_format format,
                            bool bt709, bool full_range, double pts_sec,
                            std::vector<uint8_t> &storage, avb_video_frame &out) {
    const int cw = chroma_size(w), ch = chroma_size(h);
    init_cpu_frame(out, w, h, format, pts_sec);
    auto chroma_at = [&](int x, int y, int &u, int &v) {
        if (src.nv12) {
            const uint8_t *row = src.u + (size_t)y * src.u_stride;
            u = row[x * 2];
            v = row[x * 2 + 1];
        } else {
            u = src.u[(size_t)y * src.u_stride + x];
            v = src.v[(size_t)y * src.v_stride + x];
        }
    };

    if (format == AVB_PIXEL_FORMAT_NV12 || format == AVB_PIXEL_FORMAT_I420) {
        const bool nv12 = format == AVB_PIXEL_FORMAT_NV12;
        storage.resize(yuv420_size(w, h));
        uint8_t *y_out = storage.data();
        uint8_t *c_out = storage.data() + (size_t)w * h;
        for (int y = 0; y < h; ++y)
            std::memcpy(y_out + (size_t)y * w, src.y + (size_t)y * src.y_stride, w);
        for (int y = 0; y < ch; ++y) {
            for (int x = 0; x < cw; ++x) {
                int u, v;
                chroma_at(x, y, u, v);
                if (nv12) {
                    c_out[(size_t)y * cw * 2 + x * 2] = (uint8_t)u;
                    c_out[(size_t)y * cw * 2 + x * 2 + 1] = (uint8_t)v;
                } else {
                    c_out[(size_t)y * cw + x] = (uint8_t)u;
                    c_out[(size_t)cw * ch + (size_t)y * cw + x] = (uint8_t)v;
                }
            }
        }
        out.plane_count = nv12 ? 2 : 3;
        out.plane_data[0] = y_out;
        out.plane_stride[0] = w;
        out.plane_data[1] = c_out;
        out.plane_stride[1] = nv12 ? cw * 2 : cw;
        out.plane_offset[1] = w * h;
        if (!nv12) {
            out.plane_data[2] = c_out + (size_t)cw * ch;
            out.plane_stride[2] = cw;
            out.plane_offset[2] = w * h + cw * ch;
        }
        out.color_range = full_range ? AVB_COLOR_RANGE_FULL : AVB_COLOR_RANGE_LIMITED;
        out.color_matrix = bt709 ? AVB_COLOR_MATRIX_BT709 : AVB_COLOR_MATRIX_BT601;
    } else {
        const bool rgba = format == AVB_PIXEL_FORMAT_RGBA8;
        storage.resize((size_t)w * h * 4);
        int ky, kr, kgu, kgv, kb, y_off;
        if (full_range) {
            ky = 256; y_off = 0;
            kr = bt709 ? 403 : 359; kgu = bt709 ? 48 : 88; kgv = bt709 ? 120 : 183; kb = bt709 ? 475 : 454;
        } else {
            ky = 298; y_off = 16;
            kr = bt709 ? 459 : 409; kgu = bt709 ? 55 : 100; kgv = bt709 ? 136 : 208; kb = bt709 ? 541 : 516;
        }
        for (int y = 0; y < h; ++y) {
            const uint8_t *y_row = src.y + (size_t)y * src.y_stride;
            uint8_t *dst = storage.data() + (size_t)y * w * 4;
            for (int x = 0; x < w; ++x) {
                int u, v;
                chroma_at(x / 2, y / 2, u, v);
                const int yy = ky * std::max(0, (int)y_row[x] - y_off);
                u -= 128;
                v -= 128;
                const int r = std::clamp((yy + kr * v + 128) >> 8, 0, 255);
                const int g = std::clamp((yy - kgu * u - kgv * v + 128) >> 8, 0, 255);
                const int b = std::clamp((yy + kb * u + 128) >> 8, 0, 255);
                dst[x * 4 + 0] = (uint8_t)(rgba ? r : b);
                dst[x * 4 + 1] = (uint8_t)g;
                dst[x * 4 + 2] = (uint8_t)(rgba ? b : r);
                dst[x * 4 + 3] = 255;
            }
        }
        out.plane_count = 1;
        out.plane_data[0] = storage.data();
        out.plane_stride[0] = w * 4;
    }
    out.data = out.plane_data[0];
    out.stride = out.plane_stride[0];
    out.data_size = (int)storage.size();
}

// Copy packed BGRA rows into a BGRA8 or RGBA8 CPU frame.
inline void bgra_to_frame(const uint8_t *src, int stride, int w, int h, avb_pixel_format format,
                          double pts_sec, std::vector<uint8_t> &storage, avb_video_frame &out) {
    init_cpu_frame(out, w, h, format, pts_sec);
    storage.resize((size_t)w * h * 4);
    const bool swap = format == AVB_PIXEL_FORMAT_RGBA8;
    for (int y = 0; y < h; ++y) {
        const uint8_t *row = src + (size_t)y * stride;
        uint8_t *dst = storage.data() + (size_t)y * w * 4;
        if (!swap) {
            std::memcpy(dst, row, (size_t)w * 4);
            continue;
        }
        for (int x = 0; x < w; ++x) {
            dst[x * 4 + 0] = row[x * 4 + 2];
            dst[x * 4 + 1] = row[x * 4 + 1];
            dst[x * 4 + 2] = row[x * 4 + 0];
            dst[x * 4 + 3] = row[x * 4 + 3];
        }
    }
    out.plane_count = 1;
    out.plane_data[0] = out.data = storage.data();
    out.plane_stride[0] = out.stride = w * 4;
    out.data_size = (int)storage.size();
}

} // namespace avb::detail
