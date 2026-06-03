#include "avbridge.h"
#include "avb_encoder_backend.hpp"

#include <memory>
#include <string>

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

struct avb_encoder {
    std::unique_ptr<AvbEncoderBackend> backend;
    std::string last_error;

    void set_error(const char *msg) { last_error = msg ? msg : ""; }
};

AvbEncoderBackend *avb_create_encoder_backend(avb_backend backend) {
    avb_backend resolved = backend;

    if (resolved == AVB_BACKEND_AUTO) {
#if defined(_WIN32)
        resolved = AVB_BACKEND_MEDIAFOUNDATION;
#elif defined(__APPLE__)
        resolved = AVB_BACKEND_AVFOUNDATION;
#elif defined(__linux__)
        // Linux default is GStreamer (mirrors decoding); fall back to FFmpeg if a
        // build excludes the GStreamer backend but includes FFmpeg.
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
            return new AvbEncoderMediaFoundation();
#endif
#if defined(AVB_ENABLE_AVFOUNDATION)
        case AVB_BACKEND_AVFOUNDATION:
            return new AvbEncoderAVFoundation();
#endif
#if defined(AVB_ENABLE_FFMPEG)
        case AVB_BACKEND_FFMPEG:
            return new AvbEncoderFFmpeg();
#endif
#if defined(AVB_ENABLE_GSTREAMER)
        case AVB_BACKEND_GSTREAMER:
            return new AvbEncoderGStreamer();
#endif
        default:
            return nullptr;
    }
}

extern "C" {

avb_encode_options avb_encode_options_default(void) {
    avb_encode_options o{};
    o.backend     = AVB_BACKEND_AUTO;
    o.video.codec = AVB_CODEC_AUTO;
    o.audio.codec = AVB_CODEC_AUTO;
    return o;
}

avb_result avb_encoder_open(avb_encoder **out_enc, const char *path,
                            const avb_encode_options *options) {
    if (!out_enc || !path || !options) return AVB_ERROR_INVALID_ARGUMENT;

    auto *enc = new avb_encoder();

    AvbEncoderBackend *backend = avb_create_encoder_backend(options->backend);
    if (!backend) {
        enc->set_error("Requested encoder backend is not available on this platform.");
        *out_enc = enc;
        return AVB_ERROR_BACKEND_NOT_AVAILABLE;
    }
    enc->backend.reset(backend);

    avb_result res = enc->backend->open(path, *options);
    if (res != AVB_OK) {
        const char *err = enc->backend->get_last_error();
        if (err) enc->set_error(err);
    }
    *out_enc = enc;
    return res;
}

avb_result avb_encoder_write_video(avb_encoder *enc, const avb_video_frame *frame,
                                   double pts_sec) {
    if (!enc || !frame) return AVB_ERROR_INVALID_ARGUMENT;
    if (!enc->backend) return AVB_ERROR_INVALID_ARGUMENT;
    return enc->backend->write_video(*frame, pts_sec);
}

avb_result avb_encoder_write_audio_f32(avb_encoder *enc, const float *src_interleaved,
                                       int frames) {
    if (!enc || !src_interleaved || frames <= 0) return AVB_ERROR_INVALID_ARGUMENT;
    if (!enc->backend) return AVB_ERROR_INVALID_ARGUMENT;
    return enc->backend->write_audio_f32(src_interleaved, frames);
}

avb_result avb_encoder_finish(avb_encoder *enc) {
    if (!enc || !enc->backend) return AVB_ERROR_INVALID_ARGUMENT;
    return enc->backend->finish();
}

const char *avb_encoder_get_last_error(avb_encoder *enc) {
    if (!enc) return nullptr;
    if (enc->backend) {
        const char *err = enc->backend->get_last_error();
        if (err && err[0] != '\0') return err;
    }
    return enc->last_error.empty() ? nullptr : enc->last_error.c_str();
}

void avb_encoder_close(avb_encoder *enc) {
    delete enc;
}

} // extern "C"
