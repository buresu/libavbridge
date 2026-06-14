#pragma once

#include "avbridge.h"

#include <algorithm>
#include <cstddef>
#include <sys/stat.h>
#include <unistd.h>

// DMABUF buffer-size helpers shared by the GStreamer and FFmpeg encoders when
// importing an avb_video_frame backed by dmabuf fds. POSIX-only (uses fstat);
// included solely by the encoder backends, which already require POSIX for
// dup()/close() on dmabuf fds.

// Minimum number of bytes the given plane occupies, derived from its stride,
// offset and (format-implied) row count. Used as a fallback when the dmabuf
// object's real size is unknown.
inline size_t avb_dmabuf_plane_size(const avb_video_frame &frame, int plane) {
    int rows = frame.height;
    if (frame.plane_count == 2 && plane == 1) rows = frame.height / 2;
    if (frame.plane_count == 3 && plane > 0) rows = frame.height / 2;
    if (rows <= 0 || frame.plane_stride[plane] <= 0) return 0;
    return (size_t)frame.plane_offset[plane] +
           (size_t)frame.plane_stride[plane] * (size_t)rows;
}

// Actual size of the dmabuf object behind `fd` (via fstat), or `fallback` if it
// cannot be determined or is smaller than the computed plane size.
inline size_t avb_dmabuf_object_size(int fd, size_t fallback) {
    struct stat st {};
    if (fd >= 0 && fstat(fd, &st) == 0 && st.st_size > 0)
        return std::max((size_t)st.st_size, fallback);
    return fallback;
}
