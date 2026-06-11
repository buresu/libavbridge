#include "avbridge.h"

#include <cstring>

extern "C" {

const char *avb_version_string(void) {
#define AVB_STR2(x) #x
#define AVB_STR(x) AVB_STR2(x)
    return AVB_STR(AVB_VERSION_MAJOR) "." AVB_STR(AVB_VERSION_MINOR) "."
           AVB_STR(AVB_VERSION_PATCH);
#undef AVB_STR
#undef AVB_STR2
}

const char *avb_backend_name(avb_backend backend) {
    switch (backend) {
        case AVB_BACKEND_AUTO:            return "auto";
        case AVB_BACKEND_GSTREAMER:       return "gstreamer";
        case AVB_BACKEND_FFMPEG:          return "ffmpeg";
        case AVB_BACKEND_MEDIAFOUNDATION: return "mediafoundation";
        case AVB_BACKEND_AVFOUNDATION:    return "avfoundation";
        default:                          return nullptr;
    }
}

avb_result avb_backend_from_name(const char *name, avb_backend *out) {
    if (!name || !out) return AVB_ERROR_INVALID_ARGUMENT;
    for (int b = AVB_BACKEND_AUTO; b < AVB_BACKEND_COUNT; ++b) {
        const char *n = avb_backend_name((avb_backend)b);
        if (n && std::strcmp(n, name) == 0) { *out = (avb_backend)b; return AVB_OK; }
    }
    return AVB_ERROR_INVALID_ARGUMENT;
}

int avb_backend_is_available(avb_backend backend) {
    switch (backend) {
        case AVB_BACKEND_GSTREAMER:
#if defined(AVB_ENABLE_GSTREAMER)
            return 1;
#else
            return 0;
#endif
        case AVB_BACKEND_FFMPEG:
#if defined(AVB_ENABLE_FFMPEG)
            return 1;
#else
            return 0;
#endif
        case AVB_BACKEND_MEDIAFOUNDATION:
#if defined(AVB_ENABLE_MEDIAFOUNDATION)
            return 1;
#else
            return 0;
#endif
        case AVB_BACKEND_AVFOUNDATION:
#if defined(AVB_ENABLE_AVFOUNDATION)
            return 1;
#else
            return 0;
#endif
        case AVB_BACKEND_AUTO:
            // Available if this platform has any default backend compiled in.
#if defined(_WIN32)
            return avb_backend_is_available(AVB_BACKEND_MEDIAFOUNDATION);
#elif defined(__APPLE__)
            return avb_backend_is_available(AVB_BACKEND_AVFOUNDATION);
#elif defined(__linux__)
            return avb_backend_is_available(AVB_BACKEND_GSTREAMER) ||
                   avb_backend_is_available(AVB_BACKEND_FFMPEG);
#else
            return 0;
#endif
        default:
            return 0;
    }
}

const char *avb_result_string(avb_result result) {
    switch (result) {
        case AVB_OK:                          return "AVB_OK";
        case AVB_ERROR_UNKNOWN:               return "AVB_ERROR_UNKNOWN";
        case AVB_ERROR_INVALID_ARGUMENT:      return "AVB_ERROR_INVALID_ARGUMENT";
        case AVB_ERROR_BACKEND_NOT_AVAILABLE: return "AVB_ERROR_BACKEND_NOT_AVAILABLE";
        case AVB_ERROR_OPEN_FAILED:           return "AVB_ERROR_OPEN_FAILED";
        case AVB_ERROR_STREAM_NOT_FOUND:      return "AVB_ERROR_STREAM_NOT_FOUND";
        case AVB_ERROR_DECODE_FAILED:         return "AVB_ERROR_DECODE_FAILED";
        case AVB_ERROR_SEEK_FAILED:           return "AVB_ERROR_SEEK_FAILED";
        case AVB_ERROR_EOF:                   return "AVB_ERROR_EOF";
        case AVB_ERROR_ENCODE_FAILED:         return "AVB_ERROR_ENCODE_FAILED";
        case AVB_ERROR_AGAIN:                 return "AVB_ERROR_AGAIN";
        default:                              return "AVB_ERROR_UNKNOWN";
    }
}

} // extern "C"
