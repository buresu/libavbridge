// Custom video decoder smoke test.
//
// Registers a tiny decoder that claims the fixture's video stream and returns a
// fake BC1-compressed frame from the first demuxed packet. This validates the
// demux + custom compressed-frame path without needing a real HAP file.

#include <avbridge.h>

#include <cstdio>
#include <cstring>
#include <vector>

static const int AVB_TEST_SKIP = 77;

struct DummyContext {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> bc1;
    int release_count = 0;
};

static int g_open_count = 0;
static int g_decode_count = 0;
static int g_close_count = 0;

static int can_decode(const avb_video_stream_info *stream,
                      const avb_decode_options *options) {
    return stream && options &&
           options->video_format == AVB_PIXEL_FORMAT_BC1_RGBA &&
           stream->width == 320 && stream->height == 240;
}

static avb_result open_decoder(void **out_ctx,
                               const avb_video_stream_info *stream,
                               const avb_decode_options *) {
    if (!out_ctx || !stream) return AVB_ERROR_INVALID_ARGUMENT;
    auto *ctx = new DummyContext();
    ctx->width = stream->width;
    ctx->height = stream->height;
    int blocks_x = (ctx->width + 3) / 4;
    int blocks_y = (ctx->height + 3) / 4;
    ctx->bc1.resize((size_t)blocks_x * blocks_y * 8, 0x7f);
    *out_ctx = ctx;
    ++g_open_count;
    return AVB_OK;
}

static avb_result decode_packet(void *user, const avb_encoded_packet *packet,
                                avb_video_frame *out_frame) {
    if (!user || !packet || !out_frame || !packet->data || packet->size <= 0)
        return AVB_ERROR_INVALID_ARGUMENT;
    auto *ctx = static_cast<DummyContext *>(user);
    int blocks_x = (ctx->width + 3) / 4;
    *out_frame = {};
    out_frame->width = ctx->width;
    out_frame->height = ctx->height;
    out_frame->format = AVB_PIXEL_FORMAT_BC1_RGBA;
    out_frame->pts_sec = packet->pts_sec;
    out_frame->plane_count = 1;
    out_frame->plane_data[0] = ctx->bc1.data();
    out_frame->plane_stride[0] = blocks_x * 8;
    out_frame->data = out_frame->plane_data[0];
    out_frame->stride = out_frame->plane_stride[0];
    out_frame->data_size = (int)ctx->bc1.size();
    ++g_decode_count;
    return AVB_OK;
}

static void release_frame(void *user, avb_video_frame *frame) {
    if (user) static_cast<DummyContext *>(user)->release_count++;
    if (frame) std::memset(frame, 0, sizeof(*frame));
}

static void close_decoder(void *user) {
    delete static_cast<DummyContext *>(user);
    ++g_close_count;
}

static void check(bool cond, const char *what, int *failures) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++*failures;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <fixture.mp4> [backend]\n", argv[0]);
        return 2;
    }

    avb_backend backend = AVB_BACKEND_FFMPEG;
    if (argc >= 3 && avb_backend_from_name(argv[2], &backend) != AVB_OK) {
        std::fprintf(stderr, "unknown backend '%s'\n", argv[2]);
        return 2;
    }

    if (!avb_backend_is_available(backend)) {
        std::printf("SKIP: backend '%s' not built into this library\n",
                    avb_backend_name(backend));
        return AVB_TEST_SKIP;
    }

    avb_video_decoder_plugin plugin{};
    plugin.struct_size = sizeof(plugin);
    plugin.name = "dummy-bc1";
    plugin.can_decode = can_decode;
    plugin.open = open_decoder;
    plugin.decode_packet = decode_packet;
    plugin.release_frame = release_frame;
    plugin.close = close_decoder;

    int failures = 0;
    check(avb_register_video_decoder(&plugin) == AVB_OK,
          "register custom decoder", &failures);

    avb_decode_options opts = avb_decode_options_default();
    opts.backend = backend;
    opts.enable_audio = 0;
    opts.enable_video = 1;
    opts.video_format = AVB_PIXEL_FORMAT_BC1_RGBA;

    avb_decoder *dec = nullptr;
    avb_result open_res = avb_decoder_open(&dec, argv[1], &opts);
    if (open_res == AVB_ERROR_BACKEND_NOT_AVAILABLE) {
        avb_decoder_close(dec);
        avb_unregister_video_decoder(&plugin);
        std::printf("SKIP: backend runtime libraries not available\n");
        return AVB_TEST_SKIP;
    }
    check(open_res == AVB_OK, "open backend with custom decoder", &failures);

    avb_video_frame frame{};
    avb_result read_res = avb_decoder_read_video_frame(dec, &frame);
    check(read_res == AVB_OK, "read custom video frame", &failures);
    check(frame.format == AVB_PIXEL_FORMAT_BC1_RGBA, "frame format is BC1", &failures);
    check(frame.data != nullptr && frame.data_size > 0, "frame has compressed payload", &failures);
    check(frame.stride == ((frame.width + 3) / 4) * 8,
          "BC1 stride is one compressed block row", &failures);
    avb_decoder_release_video_frame(dec, &frame);
    avb_decoder_close(dec);

    check(g_open_count == 1, "custom decoder opened once", &failures);
    check(g_decode_count >= 1, "custom decoder received packets", &failures);
    check(g_close_count == 1, "custom decoder closed once", &failures);
    check(avb_unregister_video_decoder(&plugin) == AVB_OK,
          "unregister custom decoder", &failures);

    return failures == 0 ? 0 : 1;
}
