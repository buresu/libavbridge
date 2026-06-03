#include "avb_decoder.hpp"
#include "avb_backend.hpp"

extern "C" {

avb_decode_options avb_decode_options_default(void) {
    avb_decode_options o{};
    o.backend            = AVB_BACKEND_AUTO;
    o.audio_stream_index = -1;
    o.video_stream_index = -1;
    o.enable_audio       = 1;
    o.enable_video       = 1;
    o.video_format       = AVB_PIXEL_FORMAT_UNKNOWN;
    o.audio_sample_rate  = 0;
    o.audio_channels     = 0;
    return o;
}

avb_result avb_decoder_open(avb_decoder **out_dec, const char *path,
                            const avb_decode_options *options) {
    if (!out_dec || !path) return AVB_ERROR_INVALID_ARGUMENT;

    avb_decode_options default_opts = avb_decode_options_default();
    if (!options) options = &default_opts;

    auto *dec = new avb_decoder();

    AvbBackend *backend = avb_create_backend(options->backend);
    if (!backend) {
        dec->set_error("Requested backend is not available on this platform.");
        *out_dec = dec;
        return AVB_ERROR_BACKEND_NOT_AVAILABLE;
    }
    dec->backend.reset(backend);

    avb_result res = dec->backend->open_file(path, *options);
    if (res != AVB_OK) {
        const char *err = dec->backend->get_last_error();
        if (err) dec->set_error(err);
    }
    *out_dec = dec;
    return res;
}

avb_result avb_decoder_get_media_info(avb_decoder *dec, avb_media_info *out_info) {
    if (!dec || !out_info || !dec->backend) return AVB_ERROR_INVALID_ARGUMENT;
    avb_result res = dec->backend->get_media_info(*out_info);
    if (res != AVB_OK) {
        const char *err = dec->backend->get_last_error();
        if (err) dec->set_error(err);
    }
    return res;
}

avb_result avb_decoder_seek(avb_decoder *dec, double seconds) {
    if (!dec || !dec->backend) return AVB_ERROR_INVALID_ARGUMENT;
    avb_result res = dec->backend->seek(seconds);
    if (res != AVB_OK) {
        const char *err = dec->backend->get_last_error();
        if (err) dec->set_error(err);
    }
    return res;
}

int avb_decoder_read_audio_f32(avb_decoder *dec, float *dst_interleaved, int frames) {
    if (!dec || !dst_interleaved || frames <= 0 || !dec->backend) return 0;
    return dec->backend->read_audio_f32(dst_interleaved, frames);
}

avb_result avb_decoder_read_video_frame(avb_decoder *dec, avb_video_frame *out_frame) {
    if (!dec || !out_frame || !dec->backend) return AVB_ERROR_INVALID_ARGUMENT;
    avb_result res = dec->backend->read_video_frame(*out_frame);
    if (res != AVB_OK) {
        const char *err = dec->backend->get_last_error();
        if (err) dec->set_error(err);
    }
    return res;
}

void avb_decoder_release_video_frame(avb_decoder *dec, avb_video_frame *frame) {
    if (!dec || !frame || !dec->backend) return;
    dec->backend->release_video_frame(*frame);
}

const char *avb_decoder_get_last_error(avb_decoder *dec) {
    if (!dec) return nullptr;
    if (!dec->backend) return dec->last_error.empty() ? nullptr : dec->last_error.c_str();
    const char *backend_err = dec->backend->get_last_error();
    if (backend_err && backend_err[0] != '\0') return backend_err;
    return dec->last_error.empty() ? nullptr : dec->last_error.c_str();
}

void avb_decoder_close(avb_decoder *dec) {
    delete dec;
}

} // extern "C"
