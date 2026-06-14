#pragma once

#include <cstdint>

// DRM FourCC helpers shared by the DMABUF paths of the GStreamer and FFmpeg
// backends. Portable (no platform headers); the dmabuf buffer-size helpers that
// need fstat() live in avb_dmabuf_util.hpp instead.

constexpr uint32_t avb_drm_fourcc(char a, char b, char c, char d) {
    return (uint32_t)(uint8_t)a |
           ((uint32_t)(uint8_t)b << 8) |
           ((uint32_t)(uint8_t)c << 16) |
           ((uint32_t)(uint8_t)d << 24);
}

// Render a FourCC as its 4 ASCII characters into out[5] (NUL-terminated).
// Returns false (leaving out unspecified) for zero or non-printable codes.
inline bool avb_drm_fourcc_to_string(uint32_t fourcc, char out[5]) {
    if (!fourcc) return false;
    out[0] = (char)(fourcc & 0xff);
    out[1] = (char)((fourcc >> 8) & 0xff);
    out[2] = (char)((fourcc >> 16) & 0xff);
    out[3] = (char)((fourcc >> 24) & 0xff);
    out[4] = '\0';
    for (int i = 0; i < 4; ++i) {
        unsigned char c = (unsigned char)out[i];
        if (c < 0x20 || c > 0x7e) return false;
    }
    return true;
}
