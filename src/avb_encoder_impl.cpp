#include "avb_encoder_impl.hpp"
#include "avb_backend_internal.hpp"

#if defined(AVB_ENABLE_MEDIAFOUNDATION)
#include "backends/mediafoundation/avb_encoder_mediafoundation.hpp"
#endif

#if defined(AVB_ENABLE_AVFOUNDATION)
#include "backends/avfoundation/avb_encoder_avfoundation.hh"
#endif

#if defined(AVB_ENABLE_FFMPEG)
#include "backends/ffmpeg/avb_encoder_ffmpeg.hpp"
#endif

#if defined(AVB_ENABLE_GSTREAMER)
#include "backends/gstreamer/avb_encoder_gstreamer.hpp"
#endif

AvbEncoderImpl *avb_create_encoder_impl(avb_backend backend) {
    switch (avb::detail::resolve_backend(backend)) {
#if defined(AVB_ENABLE_GSTREAMER)
        case AVB_BACKEND_GSTREAMER:
            return new AvbEncoderGStreamer();
#endif
#if defined(AVB_ENABLE_FFMPEG)
        case AVB_BACKEND_FFMPEG:
            return new AvbEncoderFFmpeg();
#endif
#if defined(AVB_ENABLE_MEDIAFOUNDATION)
        case AVB_BACKEND_MEDIAFOUNDATION:
            return new AvbEncoderMediaFoundation();
#endif
#if defined(AVB_ENABLE_AVFOUNDATION)
        case AVB_BACKEND_AVFOUNDATION:
            return new AvbEncoderAVFoundation();
#endif
        default:
            return nullptr;
    }
}
