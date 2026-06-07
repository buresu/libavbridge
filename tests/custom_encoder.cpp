// Custom video encoder smoke test.
//
// Registers a tiny encoder and verifies that FFmpeg/GStreamer can mux packets
// produced by a plugin while audio remains handled by the backend.

#include <avbridge.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static const int AVB_TEST_SKIP = 77;

struct DummyEnc {
    int width = 0;
    int height = 0;
    double frame_rate = 30.0;
    std::vector<unsigned char> packet;
};

static int g_open_count = 0;
static int g_encode_count = 0;
static int g_close_count = 0;

static uint32_t fourcc_le(const char *s) {
    return ((uint32_t)(unsigned char)s[0]) |
           ((uint32_t)(unsigned char)s[1] << 8) |
           ((uint32_t)(unsigned char)s[2] << 16) |
           ((uint32_t)(unsigned char)s[3] << 24);
}

static int can_encode(const avb_video_encode_info *info) {
    return info && info->codec == AVB_CODEC_HAP;
}

static avb_result open_encoder(void **out_ctx,
                               const avb_video_encode_info *info,
                               avb_encoded_video_stream *out_stream) {
    if (!out_ctx || !info || !out_stream) return AVB_ERROR_INVALID_ARGUMENT;
    auto *ctx = new DummyEnc();
    ctx->width = info->width;
    ctx->height = info->height;
    ctx->frame_rate = info->frame_rate > 0.0 ? info->frame_rate : 30.0;
    *out_ctx = ctx;

    *out_stream = {};
    out_stream->codec = AVB_CODEC_HAP;
    out_stream->codec_tag = fourcc_le("Hap1");
    out_stream->codec_name = "hap";
    out_stream->gst_caps = "video/x-raw,format=RGB,width=%d,height=%d,framerate=%d/1";
    out_stream->time_base_num = 1;
    out_stream->time_base_den = (int)ctx->frame_rate;
    ++g_open_count;
    return AVB_OK;
}

static avb_result encode_frame(void *user, const avb_video_frame *frame,
                               double pts_sec, avb_encoded_packet *out_packet) {
    if (!user || !frame || !out_packet || !frame->data)
        return AVB_ERROR_INVALID_ARGUMENT;
    auto *ctx = static_cast<DummyEnc *>(user);
    ctx->packet.resize((size_t)ctx->width * ctx->height * 3);
    for (int y = 0; y < ctx->height; ++y) {
        const unsigned char *src = frame->data + (size_t)y * frame->stride;
        unsigned char *dst = ctx->packet.data() + (size_t)y * ctx->width * 3;
        for (int x = 0; x < ctx->width; ++x) {
            dst[x * 3 + 0] = src[x * 4 + 2];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 0];
        }
    }

    *out_packet = {};
    out_packet->data = ctx->packet.data();
    out_packet->size = (int)ctx->packet.size();
    out_packet->pts_sec = pts_sec >= 0.0 ? pts_sec : frame->pts_sec;
    out_packet->duration_sec = 1.0 / ctx->frame_rate;
    out_packet->keyframe = 1;
    out_packet->pts = -1;
    out_packet->dts = -1;
    out_packet->duration = -1;
    ++g_encode_count;
    return AVB_OK;
}

static avb_result flush_encoder(void *, avb_encoded_packet *) {
    return AVB_ERROR_EOF;
}

static void close_encoder(void *user) {
    delete static_cast<DummyEnc *>(user);
    ++g_close_count;
}

static void check(bool cond, const char *what, int *failures) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++*failures;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <out.mov> <backend>\n", argv[0]);
        return 2;
    }

    avb_backend backend = AVB_BACKEND_AUTO;
    if (avb_backend_from_name(argv[2], &backend) != AVB_OK) {
        std::fprintf(stderr, "unknown backend '%s'\n", argv[2]);
        return 2;
    }
    if (!avb_backend_is_available(backend)) {
        std::printf("SKIP: backend '%s' not built into this library\n", argv[2]);
        return AVB_TEST_SKIP;
    }

    avb_video_encoder_plugin plugin{};
    plugin.struct_size = sizeof(plugin);
    plugin.name = "dummy-custom-encoder";
    plugin.can_encode = can_encode;
    plugin.open = open_encoder;
    plugin.encode_frame = encode_frame;
    plugin.flush = flush_encoder;
    plugin.close = close_encoder;

    int failures = 0;
    check(avb_register_video_encoder(&plugin) == AVB_OK,
          "register custom encoder", &failures);

    const int W = 32, H = 32;
    avb_encode_options opts = avb_encode_options_default();
    opts.backend = backend;
    opts.video.enable = 1;
    opts.video.width = W;
    opts.video.height = H;
    opts.video.frame_rate = 30.0;
    opts.video.codec = AVB_CODEC_HAP;
    opts.video.input_format = AVB_PIXEL_FORMAT_BGRA8;

    avb_encoder *enc = nullptr;
    avb_result open_res = avb_encoder_open(&enc, argv[1], &opts);
    if (open_res == AVB_ERROR_BACKEND_NOT_AVAILABLE) {
        avb_encoder_close(enc);
        avb_unregister_video_encoder(&plugin);
        std::printf("SKIP: backend runtime libraries not available\n");
        return AVB_TEST_SKIP;
    }
    check(open_res == AVB_OK, "open backend with custom encoder", &failures);

    std::vector<unsigned char> pixels((size_t)W * H * 4, 0);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            unsigned char *p = pixels.data() + ((size_t)y * W + x) * 4;
            p[0] = (unsigned char)(x * 8);
            p[1] = (unsigned char)(y * 8);
            p[2] = 0x80;
            p[3] = 0xff;
        }
    }
    avb_video_frame frame{};
    frame.width = W;
    frame.height = H;
    frame.format = AVB_PIXEL_FORMAT_BGRA8;
    frame.pts_sec = 0.0;
    frame.plane_count = 1;
    frame.plane_data[0] = pixels.data();
    frame.plane_stride[0] = W * 4;
    frame.data = pixels.data();
    frame.stride = W * 4;
    frame.data_size = (int)pixels.size();

    if (open_res == AVB_OK) {
        check(avb_encoder_write_video(enc, &frame, 0.0) == AVB_OK,
              "write custom video packet", &failures);
        avb_result finish_res = avb_encoder_finish(enc);
        if (finish_res != AVB_OK) {
            std::printf("    finish error: %s\n",
                        avb_encoder_get_last_error(enc)
                            ? avb_encoder_get_last_error(enc) : "(none)");
        }
        check(finish_res == AVB_OK, "finish custom encoder mux", &failures);
    }
    avb_encoder_close(enc);

    check(g_open_count == 1, "custom encoder opened once", &failures);
    check(g_encode_count == 1, "custom encoder received frame", &failures);
    check(g_close_count == 1, "custom encoder closed once", &failures);
    check(avb_unregister_video_encoder(&plugin) == AVB_OK,
          "unregister custom encoder", &failures);
    return failures == 0 ? 0 : 1;
}
