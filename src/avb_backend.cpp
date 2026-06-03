#include "avb_backend.hpp"

#if defined(AVB_ENABLE_FFMPEG)
#include "backends/ffmpeg/avb_backend_ffmpeg.hpp"
#endif

#if defined(AVB_ENABLE_MEDIAFOUNDATION)
#include "backends/mediafoundation/avb_backend_mediafoundation.hpp"
#endif

#if defined(AVB_ENABLE_AVFOUNDATION)
#include "backends/avfoundation/avb_backend_avfoundation.hh"
#endif

#if defined(AVB_ENABLE_GSTREAMER)
#include "backends/gstreamer/avb_backend_gstreamer.hpp"
#endif

AvbBackend *avb_create_backend(avb_backend backend) {
    avb_backend resolved = backend;

    if (resolved == AVB_BACKEND_AUTO) {
#if defined(_WIN32)
        resolved = AVB_BACKEND_MEDIAFOUNDATION;
#elif defined(__APPLE__)
        resolved = AVB_BACKEND_AVFOUNDATION;
#elif defined(__linux__)
        // Linux default is GStreamer. If a build excludes the GStreamer backend
        // but includes FFmpeg, fall back to FFmpeg so AUTO still resolves.
#  if defined(AVB_ENABLE_GSTREAMER)
        resolved = AVB_BACKEND_GSTREAMER;
#  elif defined(AVB_ENABLE_FFMPEG)
        resolved = AVB_BACKEND_FFMPEG;
#  else
        return nullptr;
#  endif
#else
        return nullptr;
#endif
    }

    switch (resolved) {
#if defined(AVB_ENABLE_MEDIAFOUNDATION)
        case AVB_BACKEND_MEDIAFOUNDATION:
            return new AvbBackendMediaFoundation();
#endif
#if defined(AVB_ENABLE_AVFOUNDATION)
        case AVB_BACKEND_AVFOUNDATION:
            return new AvbBackendAVFoundation();
#endif
#if defined(AVB_ENABLE_FFMPEG)
        case AVB_BACKEND_FFMPEG:
            return new AvbBackendFFmpeg();
#endif
#if defined(AVB_ENABLE_GSTREAMER)
        case AVB_BACKEND_GSTREAMER:
            return new AvbBackendGStreamer();
#endif
        default:
            return nullptr;
    }
}
