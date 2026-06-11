#include "avbridge.h"

#include <cstdio>
#include <cstring>

namespace {

static void copy_string(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) return;
    if (!src) src = "";
    std::snprintf(dst, dst_size, "%s", src);
}

static void copy_media_info(avb_media_probe &dst, const avb_media_info &src) {
    copy_string(dst.backend_name, sizeof(dst.backend_name), src.backend_name);
    dst.duration_sec = src.duration_sec;

    dst.audio.available = src.audio.available;
    dst.audio.stream_index = src.audio.stream_index;
    dst.audio.track_count = src.audio.track_count;
    dst.audio.sample_rate = src.audio.sample_rate;
    dst.audio.channels = src.audio.channels;
    dst.audio.duration_sec = src.audio.duration_sec;
    copy_string(dst.audio.codec_name, sizeof(dst.audio.codec_name), src.audio.codec_name);

    dst.video.available = src.video.available;
    dst.video.stream_index = src.video.stream_index;
    dst.video.width = src.video.width;
    dst.video.height = src.video.height;
    dst.video.frame_rate = src.video.frame_rate;
    dst.video.duration_sec = src.video.duration_sec;
    copy_string(dst.video.codec_name, sizeof(dst.video.codec_name), src.video.codec_name);
}

} // namespace

extern "C" {

avb_result avb_probe_media(const char *path,
                           const avb_decode_options *options,
                           avb_media_probe *out_probe) {
    if (!path || !out_probe) return AVB_ERROR_INVALID_ARGUMENT;
    *out_probe = {};

    avb_decoder *dec = nullptr;
    avb_result res = avb_decoder_open(&dec, path, options);
    out_probe->result = res;
    if (res != AVB_OK) {
        const char *err = avb_decoder_get_last_error(dec);
        copy_string(out_probe->error, sizeof(out_probe->error),
                    err ? err : avb_result_string(res));
        avb_decoder_close(dec);
        return res;
    }

    avb_media_info info{};
    res = avb_decoder_get_media_info(dec, &info);
    out_probe->result = res;
    if (res == AVB_OK) {
        copy_media_info(*out_probe, info);
    } else {
        const char *err = avb_decoder_get_last_error(dec);
        copy_string(out_probe->error, sizeof(out_probe->error),
                    err ? err : avb_result_string(res));
    }
    avb_decoder_close(dec);
    return res;
}

} // extern "C"
