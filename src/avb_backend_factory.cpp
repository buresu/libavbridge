#include "avb_backend.hpp"
#include "avb_decoder_impl.hpp"
#include "avb_encoder_impl.hpp"

#if defined(AVB_ENABLE_AVFOUNDATION)
#include "backends/avfoundation/avb_decoder_avfoundation.hh"
#include "backends/avfoundation/avb_encoder_avfoundation.hh"
#endif

#if defined(AVB_ENABLE_FFMPEG)
#include "backends/ffmpeg/avb_decoder_ffmpeg.hpp"
#include "backends/ffmpeg/avb_encoder_ffmpeg.hpp"
#endif

#if defined(AVB_ENABLE_GSTREAMER)
#include "backends/gstreamer/avb_decoder_gstreamer.hpp"
#include "backends/gstreamer/avb_encoder_gstreamer.hpp"
#endif

#if defined(AVB_ENABLE_MEDIAFOUNDATION)
#include "backends/mediafoundation/avb_decoder_mediafoundation.hpp"
#include "backends/mediafoundation/avb_encoder_mediafoundation.hpp"
#endif

std::unique_ptr<AvbDecoderImpl> avb_create_decoder_backend(avb_backend backend) {
    switch (avb::detail::resolve_backend(backend)) {
#if defined(AVB_ENABLE_AVFOUNDATION)
        case AVB_BACKEND_AVFOUNDATION:
            return std::make_unique<AvbDecoderAVFoundation>();
#endif
#if defined(AVB_ENABLE_FFMPEG)
        case AVB_BACKEND_FFMPEG:
            return std::make_unique<AvbDecoderFFmpeg>();
#endif
#if defined(AVB_ENABLE_GSTREAMER)
        case AVB_BACKEND_GSTREAMER:
            return std::make_unique<AvbDecoderGStreamer>();
#endif
#if defined(AVB_ENABLE_MEDIAFOUNDATION)
        case AVB_BACKEND_MEDIAFOUNDATION:
            return std::make_unique<AvbDecoderMediaFoundation>();
#endif
        default:
            return nullptr;
    }
}

std::unique_ptr<AvbEncoderImpl> avb_create_encoder_backend(avb_backend backend) {
    switch (avb::detail::resolve_backend(backend)) {
#if defined(AVB_ENABLE_AVFOUNDATION)
        case AVB_BACKEND_AVFOUNDATION:
            return std::make_unique<AvbEncoderAVFoundation>();
#endif
#if defined(AVB_ENABLE_FFMPEG)
        case AVB_BACKEND_FFMPEG:
            return std::make_unique<AvbEncoderFFmpeg>();
#endif
#if defined(AVB_ENABLE_GSTREAMER)
        case AVB_BACKEND_GSTREAMER:
            return std::make_unique<AvbEncoderGStreamer>();
#endif
#if defined(AVB_ENABLE_MEDIAFOUNDATION)
        case AVB_BACKEND_MEDIAFOUNDATION:
            return std::make_unique<AvbEncoderMediaFoundation>();
#endif
        default:
            return nullptr;
    }
}
