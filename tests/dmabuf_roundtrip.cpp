// Optional DMABUF hardware round-trip smoke test.
//
// Usage:
//   avb_dmabuf_roundtrip <fixture.mp4> <output.mp4> <decode_backend> <encode_backend>
//
// This test intentionally skips when hardware/driver support is unavailable. If
// DMABUF decode and encode both open and a frame is produced, write/finish/redecode
// failures are treated as real failures.

#include <avbridge.h>

#include <cstdio>
#include <cstring>

static const int AVB_TEST_SKIP = 77;

static bool parse_backend(const char *name, avb_backend *out) {
    if (avb_backend_from_name(name, out) != AVB_OK) {
        fprintf(stderr, "unknown backend '%s'\n", name);
        return false;
    }
    if (!avb_backend_is_available(*out)) {
        printf("SKIP: backend '%s' not built into this library\n", name);
        return false;
    }
    return true;
}

static bool backend_missing(avb_backend backend) {
    return !avb_backend_is_available(backend);
}

static bool is_optional_runtime_unsupported(const char *error) {
    if (!error) return false;
    return std::strstr(error, "Function not implemented") ||
           std::strstr(error, "not implemented") ||
           std::strstr(error, "not supported") ||
           std::strstr(error, "unsupported");
}

int main(int argc, char *argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <fixture.mp4> <output.mp4> <decode_backend> <encode_backend>\n", argv[0]);
        return 2;
    }

    const char *in_path = argv[1];
    const char *out_path = argv[2];
    avb_backend decode_backend = AVB_BACKEND_AUTO;
    avb_backend encode_backend = AVB_BACKEND_AUTO;

    if (!parse_backend(argv[3], &decode_backend) ||
        !parse_backend(argv[4], &encode_backend)) {
        return AVB_TEST_SKIP;
    }
    if (backend_missing(decode_backend) || backend_missing(encode_backend))
        return AVB_TEST_SKIP;

    avb_decode_options dopts = avb_decode_options_default();
    dopts.backend = decode_backend;
    dopts.enable_audio = 0;
    dopts.enable_video = 1;
    dopts.video_format = AVB_PIXEL_FORMAT_UNKNOWN;
    dopts.video_memory = AVB_VIDEO_MEMORY_DMABUF;
    dopts.hardware_policy = AVB_HARDWARE_PREFER;
    dopts.hardware_device = AVB_HW_DEVICE_VAAPI;

    avb_decoder *dec = nullptr;
    avb_result dres = avb_decoder_open(&dec, in_path, &dopts);
    if (dres != AVB_OK) {
        printf("SKIP: DMABUF decoder open failed for %s: %s\n", argv[3],
               avb_decoder_get_last_error(dec) ? avb_decoder_get_last_error(dec) : "unknown");
        avb_decoder_close(dec);
        return AVB_TEST_SKIP;
    }

    avb_media_info mi{};
    if (avb_decoder_get_media_info(dec, &mi) != AVB_OK || !mi.video.available) {
        printf("SKIP: no video stream for DMABUF smoke\n");
        avb_decoder_close(dec);
        return AVB_TEST_SKIP;
    }

    avb_video_frame first{};
    avb_result r = avb_decoder_read_video_frame(dec, &first);
    if (r != AVB_OK || first.memory_type != AVB_VIDEO_MEMORY_DMABUF ||
        first.plane_count <= 0 || first.dmabuf_fd[0] < 0) {
        printf("SKIP: first DMABUF frame unavailable from %s: %s\n", argv[3],
               avb_decoder_get_last_error(dec) ? avb_decoder_get_last_error(dec) : "unknown");
        avb_decoder_release_video_frame(dec, &first);
        avb_decoder_close(dec);
        return AVB_TEST_SKIP;
    }
    printf("DMABUF first frame: %dx%d pts=%.6f planes=%d drm=0x%08x modifier=0x%016llx "
           "fd0=%d fd1=%d stride0=%d offset0=%d stride1=%d offset1=%d\n",
           first.width, first.height, first.pts_sec, first.plane_count, first.drm_format,
           (unsigned long long)first.dmabuf_modifier[0],
           first.dmabuf_fd[0], first.dmabuf_fd[1],
           first.plane_stride[0], first.plane_offset[0],
           first.plane_stride[1], first.plane_offset[1]);

    avb_encode_options eopts = avb_encode_options_default();
    eopts.backend = encode_backend;
    eopts.video.enable = 1;
    eopts.video.width = first.width;
    eopts.video.height = first.height;
    eopts.video.frame_rate = mi.video.frame_rate > 0.0 ? mi.video.frame_rate : 25.0;
    eopts.video.codec = AVB_CODEC_H264;
    eopts.video.input_format = AVB_PIXEL_FORMAT_UNKNOWN;
    eopts.video.input_memory = AVB_VIDEO_MEMORY_DMABUF;
    eopts.video.hardware_policy = AVB_HARDWARE_PREFER;
    eopts.video.hardware_device = AVB_HW_DEVICE_VAAPI;

    avb_encoder *enc = nullptr;
    avb_result eres = avb_encoder_open(&enc, out_path, &eopts);
    if (eres != AVB_OK) {
        printf("SKIP: DMABUF encoder open failed for %s: %s\n", argv[4],
               avb_encoder_get_last_error(enc) ? avb_encoder_get_last_error(enc) : "unknown");
        avb_encoder_close(enc);
        avb_decoder_release_video_frame(dec, &first);
        avb_decoder_close(dec);
        return AVB_TEST_SKIP;
    }

    int written = 0;
    avb_video_frame frame = first;
    bool have_frame = true;
    while (have_frame && written < 8) {
        if (avb_encoder_write_video(enc, &frame, frame.pts_sec) != AVB_OK) {
            const char *error = avb_encoder_get_last_error(enc);
            if (written == 0 && is_optional_runtime_unsupported(error)) {
                printf("SKIP: DMABUF encoder write unsupported for %s: %s\n",
                       argv[4], error ? error : "unknown");
                avb_decoder_release_video_frame(dec, &frame);
                avb_encoder_close(enc);
                avb_decoder_close(dec);
                return AVB_TEST_SKIP;
            }
            fprintf(stderr, "write_video failed: %s\n", error ? error : "unknown");
            avb_decoder_release_video_frame(dec, &frame);
            avb_encoder_close(enc);
            avb_decoder_close(dec);
            return 1;
        }
        ++written;
        avb_decoder_release_video_frame(dec, &frame);
        have_frame = avb_decoder_read_video_frame(dec, &frame) == AVB_OK;
    }

    if (have_frame) avb_decoder_release_video_frame(dec, &frame);
    avb_decoder_close(dec);

    if (written == 0) {
        printf("SKIP: no DMABUF frames written\n");
        avb_encoder_close(enc);
        return AVB_TEST_SKIP;
    }

    if (avb_encoder_finish(enc) != AVB_OK) {
        fprintf(stderr, "encoder finish failed: %s\n",
                avb_encoder_get_last_error(enc) ? avb_encoder_get_last_error(enc) : "unknown");
        avb_encoder_close(enc);
        return 1;
    }
    avb_encoder_close(enc);

    avb_decode_options verify_opts = avb_decode_options_default();
    verify_opts.backend = encode_backend;
    verify_opts.enable_audio = 0;
    verify_opts.enable_video = 1;
    verify_opts.video_format = AVB_PIXEL_FORMAT_BGRA8;
    verify_opts.video_memory = AVB_VIDEO_MEMORY_CPU;

    avb_decoder *verify = nullptr;
    if (avb_decoder_open(&verify, out_path, &verify_opts) != AVB_OK) {
        fprintf(stderr, "re-open encoded output failed: %s\n",
                avb_decoder_get_last_error(verify) ? avb_decoder_get_last_error(verify) : "unknown");
        avb_decoder_close(verify);
        return 1;
    }
    avb_video_frame vf{};
    bool decoded = avb_decoder_read_video_frame(verify, &vf) == AVB_OK;
    avb_decoder_release_video_frame(verify, &vf);
    avb_decoder_close(verify);

    if (!decoded) {
        fprintf(stderr, "encoded output did not decode a video frame\n");
        return 1;
    }

    printf("DMABUF roundtrip passed: %s -> %s, %d frames\n",
           argv[3], argv[4], written);
    return 0;
}
