#pragma once

#include "avbridge.h"

namespace avb::detail {

inline avb_backend resolve_backend(avb_backend backend) {
    if (backend != AVB_BACKEND_AUTO) return backend;
#if defined(_WIN32)
    return AVB_BACKEND_MEDIAFOUNDATION;
#elif defined(__APPLE__)
    return AVB_BACKEND_AVFOUNDATION;
#elif defined(__linux__)
#  if defined(AVB_ENABLE_GSTREAMER)
    return AVB_BACKEND_GSTREAMER;
#  elif defined(AVB_ENABLE_FFMPEG)
    return AVB_BACKEND_FFMPEG;
#  else
    return AVB_BACKEND_AUTO;
#  endif
#else
    return AVB_BACKEND_AUTO;
#endif
}

}  // namespace avb::detail
