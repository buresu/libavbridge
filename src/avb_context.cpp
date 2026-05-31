#include "avb_context.hpp"
#include "avb_backend.hpp"
#include <cstring>

extern "C" {

avb_result avb_open_file(avb_context **out_ctx, const char *path, const avb_open_options *options) {
    if (!out_ctx || !path) {
        return AVB_ERROR_INVALID_ARGUMENT;
    }

    avb_open_options default_opts{};
    default_opts.backend = AVB_BACKEND_AUTO;
    default_opts.audio_stream_index = -1;
    default_opts.video_stream_index = -1;
    default_opts.enable_audio = 1;
    default_opts.enable_video = 0;
    if (!options) {
        options = &default_opts;
    }

    auto *ctx = new avb_context();

    AvbBackend *backend = avb_create_backend(options->backend);
    if (!backend) {
        ctx->set_error("Requested backend is not available on this platform.");
        *out_ctx = ctx;
        return AVB_ERROR_BACKEND_NOT_AVAILABLE;
    }
    ctx->backend.reset(backend);

    avb_result res = ctx->backend->open_file(path, *options);
    if (res != AVB_OK) {
        const char *err = ctx->backend->get_last_error();
        if (err) ctx->set_error(err);
        *out_ctx = ctx;
        return res;
    }

    *out_ctx = ctx;
    return AVB_OK;
}

avb_result avb_get_media_info(avb_context *ctx, avb_media_info *out_info) {
    if (!ctx || !out_info) return AVB_ERROR_INVALID_ARGUMENT;
    if (!ctx->backend) return AVB_ERROR_INVALID_ARGUMENT;
    avb_result res = ctx->backend->get_media_info(*out_info);
    if (res != AVB_OK) {
        const char *err = ctx->backend->get_last_error();
        if (err) ctx->set_error(err);
    }
    return res;
}

avb_result avb_seek(avb_context *ctx, double seconds) {
    if (!ctx) return AVB_ERROR_INVALID_ARGUMENT;
    if (!ctx->backend) return AVB_ERROR_INVALID_ARGUMENT;
    avb_result res = ctx->backend->seek(seconds);
    if (res != AVB_OK) {
        const char *err = ctx->backend->get_last_error();
        if (err) ctx->set_error(err);
    }
    return res;
}

int avb_read_audio_f32(avb_context *ctx, float *dst_interleaved, int frames) {
    if (!ctx || !dst_interleaved || frames <= 0) return 0;
    if (!ctx->backend) return 0;
    return ctx->backend->read_audio_f32(dst_interleaved, frames);
}

avb_result avb_read_video_frame(avb_context *ctx, avb_video_frame *out_frame) {
    if (!ctx || !out_frame) return AVB_ERROR_INVALID_ARGUMENT;
    if (!ctx->backend) return AVB_ERROR_INVALID_ARGUMENT;
    avb_result res = ctx->backend->read_video_frame(*out_frame);
    if (res != AVB_OK) {
        const char *err = ctx->backend->get_last_error();
        if (err) ctx->set_error(err);
    }
    return res;
}

void avb_release_video_frame(avb_context *ctx, avb_video_frame *frame) {
    if (!ctx || !frame) return;
    if (!ctx->backend) return;
    ctx->backend->release_video_frame(*frame);
}

const char *avb_get_last_error(avb_context *ctx) {
    if (!ctx) return nullptr;
    if (!ctx->backend) return ctx->last_error.empty() ? nullptr : ctx->last_error.c_str();
    const char *backend_err = ctx->backend->get_last_error();
    if (backend_err && backend_err[0] != '\0') return backend_err;
    return ctx->last_error.empty() ? nullptr : ctx->last_error.c_str();
}

void avb_close(avb_context *ctx) {
    delete ctx;
}

} // extern "C"
