#include "avb_decoder.hpp"
#include "avb_backend.hpp"

#include <cstdio>
#include <cstring>

namespace {

// Backing context for avb_decoder_open_memory: a borrowed byte buffer plus a
// read cursor. The buffer is NOT owned (caller keeps it alive until close).
struct MemIo {
    const unsigned char *data;
    long long size;
    long long pos;
};

int mem_read(void *user, unsigned char *buf, int size) {
    auto *m = static_cast<MemIo *>(user);
    long long remaining = m->size - m->pos;
    if (remaining <= 0) return 0;
    int n = (size < remaining) ? size : (int)remaining;
    std::memcpy(buf, m->data + m->pos, (size_t)n);
    m->pos += n;
    return n;
}

long long mem_seek(void *user, long long offset, int whence) {
    auto *m = static_cast<MemIo *>(user);
    long long base = (whence == SEEK_CUR) ? m->pos
                   : (whence == SEEK_END) ? m->size
                                          : 0; // SEEK_SET
    long long np = base + offset;
    if (np < 0 || np > m->size) return -1;
    m->pos = np;
    return np;
}

long long mem_size(void *user) {
    return static_cast<MemIo *>(user)->size;
}

// After a successful open, cache the bits the C layer needs for seek-clamp and
// audio-EOF distinction.
void cache_media(avb_decoder *dec) {
    avb_media_info info{};
    if (dec->backend->get_media_info(info) == AVB_OK) {
        dec->duration_sec    = info.duration_sec;
        dec->audio_available = info.audio.available != 0;
    }
}

} // namespace

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

// Allocate a decoder and its backend, or report why not. The caller-supplied
// `open` lambda performs the actual open against the backend.
static avb_result decoder_create(avb_decoder **out_dec, avb_backend be,
                                 avb_decoder **dec_out) {
    auto *dec = new avb_decoder();
    AvbBackend *backend = avb_create_backend(be);
    if (!backend) {
        dec->set_error("Requested backend is not available on this platform.");
        *out_dec = dec;
        return AVB_ERROR_BACKEND_NOT_AVAILABLE;
    }
    dec->backend.reset(backend);
    *dec_out = dec;
    return AVB_OK;
}

avb_result avb_decoder_open(avb_decoder **out_dec, const char *path,
                            const avb_decode_options *options) {
    if (!out_dec || !path) return AVB_ERROR_INVALID_ARGUMENT;
    avb_decode_options default_opts = avb_decode_options_default();
    if (!options) options = &default_opts;

    avb_decoder *dec = nullptr;
    avb_result res = decoder_create(out_dec, options->backend, &dec);
    if (res != AVB_OK) return res;

    res = dec->backend->open_file(path, *options);
    if (res != AVB_OK) {
        const char *err = dec->backend->get_last_error();
        if (err) dec->set_error(err);
    } else {
        cache_media(dec);
    }
    *out_dec = dec;
    return res;
}

avb_result avb_decoder_open_io(avb_decoder **out_dec, const avb_io_callbacks *cb,
                               void *user, const avb_decode_options *options) {
    if (!out_dec || !cb || !cb->read) return AVB_ERROR_INVALID_ARGUMENT;
    avb_decode_options default_opts = avb_decode_options_default();
    if (!options) options = &default_opts;

    avb_decoder *dec = nullptr;
    avb_result res = decoder_create(out_dec, options->backend, &dec);
    if (res != AVB_OK) return res;

    res = dec->backend->open_io(*cb, user, *options);
    if (res != AVB_OK) {
        const char *err = dec->backend->get_last_error();
        if (err) dec->set_error(err);
    } else {
        cache_media(dec);
    }
    *out_dec = dec;
    return res;
}

avb_result avb_decoder_open_memory(avb_decoder **out_dec, const void *data,
                                   size_t size, const avb_decode_options *options) {
    if (!out_dec || !data) return AVB_ERROR_INVALID_ARGUMENT;

    auto *m = new MemIo{ static_cast<const unsigned char *>(data), (long long)size, 0 };
    avb_io_callbacks cb{};
    cb.read = mem_read;
    cb.seek = mem_seek;
    cb.size = mem_size;

    avb_result res = avb_decoder_open_io(out_dec, &cb, m, options);
    // *out_dec is always set by open_io; attach the MemIo so it is freed on close.
    if (*out_dec) (*out_dec)->mem_io = m;
    else          delete m;
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

avb_result avb_decoder_seek(avb_decoder *dec, double seconds, double *out_landed_sec) {
    if (!dec || !dec->backend) return AVB_ERROR_INVALID_ARGUMENT;

    // Clamp to [0, duration] before seeking: a past-the-end request can leave
    // some backends' pipelines wedged at EOS.
    double target = seconds < 0.0 ? 0.0 : seconds;
    if (dec->duration_sec > 0.0 && target > dec->duration_sec)
        target = dec->duration_sec;

    avb_result res = dec->backend->seek(target);
    if (res != AVB_OK) {
        const char *err = dec->backend->get_last_error();
        if (err) dec->set_error(err);
        return res;
    }
    dec->audio_eof = false;
    if (out_landed_sec) *out_landed_sec = target;
    return AVB_OK;
}

int avb_decoder_read_audio_f32(avb_decoder *dec, float *dst_interleaved, int frames,
                               double *out_first_pts) {
    if (!dec || !dst_interleaved || frames <= 0 || !dec->backend) {
        if (out_first_pts) *out_first_pts = -1.0;
        return 0;
    }
    if (out_first_pts) *out_first_pts = dec->backend->audio_next_pts();
    int got = dec->backend->read_audio_f32(dst_interleaved, frames);
    if (got == 0 && dec->audio_available) dec->audio_eof = true;
    return got;
}

int avb_decoder_audio_at_eof(avb_decoder *dec) {
    if (!dec) return 0;
    return (dec->audio_available && dec->audio_eof) ? 1 : 0;
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
    if (!dec) return;
    dec->backend.reset(); // close backend (and remove any temp spill) first
    delete static_cast<MemIo *>(dec->mem_io);
    delete dec;
}

} // extern "C"
