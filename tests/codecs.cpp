// Codec / pixel-format coverage for the FFmpeg backend (the one with the widest
// codec support). Round-trips the fixture through HEVC, VP9 and Opus, and feeds
// I420 frames into the encoder, then re-opens each output and asserts the codec
// and decodability survived. Skips cleanly if the FFmpeg backend is not built
// in or its runtime libraries are unavailable.
//
// Usage: avb_codecs <fixture.mp4> <out_prefix>

#include <avbridge.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static const int AVB_TEST_SKIP = 77;
static int g_failures = 0;

static void check(bool cond, const char *what) {
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_failures;
}

// Transcode the fixture's video track through `vc`, reading frames as `infmt`.
static bool transcode_video(const char *in, const char *out, avb_codec vc,
                            avb_pixel_format infmt) {
    avb_decode_options dop = avb_decode_options_default();
    dop.backend = AVB_BACKEND_FFMPEG;
    dop.enable_audio = 0; dop.enable_video = 1; dop.video_format = infmt;
    avb_decoder *d = nullptr;
    if (avb_decoder_open(&d, in, &dop) != AVB_OK) { avb_decoder_close(d); return false; }
    avb_media_info mi{}; avb_decoder_get_media_info(d, &mi);

    avb_encode_options eo = avb_encode_options_default();
    eo.backend = AVB_BACKEND_FFMPEG;
    eo.video.enable = 1; eo.video.width = mi.video.width; eo.video.height = mi.video.height;
    eo.video.frame_rate = mi.video.frame_rate > 0 ? mi.video.frame_rate : 25.0;
    eo.video.codec = vc; eo.video.input_format = infmt;
    avb_encoder *e = nullptr;
    if (avb_encoder_open(&e, out, &eo) != AVB_OK) {
        printf("    enc open: %s\n", avb_encoder_get_last_error(e));
        avb_encoder_close(e); avb_decoder_close(d); return false;
    }
    avb_video_frame f{};
    while (avb_decoder_read_video_frame(d, &f) == AVB_OK) {
        avb_encoder_write_video(e, &f, f.pts_sec);
        avb_decoder_release_video_frame(d, &f);
    }
    bool ok = avb_encoder_finish(e) == AVB_OK;
    if (!ok) printf("    finish: %s\n", avb_encoder_get_last_error(e));
    avb_encoder_close(e); avb_decoder_close(d);
    return ok;
}

static bool transcode_audio(const char *in, const char *out, avb_codec ac, int rate) {
    avb_decode_options dop = avb_decode_options_default();
    dop.backend = AVB_BACKEND_FFMPEG;
    dop.enable_audio = 1; dop.enable_video = 0;
    dop.audio_sample_rate = rate; dop.audio_channels = 2;
    avb_decoder *d = nullptr;
    if (avb_decoder_open(&d, in, &dop) != AVB_OK) { avb_decoder_close(d); return false; }
    avb_media_info mi{}; avb_decoder_get_media_info(d, &mi);

    avb_encode_options eo = avb_encode_options_default();
    eo.backend = AVB_BACKEND_FFMPEG;
    eo.audio.enable = 1; eo.audio.sample_rate = mi.audio.sample_rate;
    eo.audio.channels = mi.audio.channels; eo.audio.codec = ac;
    avb_encoder *e = nullptr;
    if (avb_encoder_open(&e, out, &eo) != AVB_OK) {
        printf("    enc open: %s\n", avb_encoder_get_last_error(e));
        avb_encoder_close(e); avb_decoder_close(d); return false;
    }
    std::vector<float> buf(4096 * mi.audio.channels);
    for (;;) {
        int g = avb_decoder_read_audio_f32(d, buf.data(), 4096, nullptr);
        if (g <= 0) break;
        if (avb_encoder_write_audio_f32(e, buf.data(), g) != AVB_OK) break;
    }
    bool ok = avb_encoder_finish(e) == AVB_OK;
    if (!ok) printf("    finish: %s\n", avb_encoder_get_last_error(e));
    avb_encoder_close(e); avb_decoder_close(d);
    return ok;
}

// Re-open `path` with FFmpeg and return the audio/video codec name.
static std::string probe_codec(const char *path, bool video) {
    avb_decode_options o = avb_decode_options_default();
    o.backend = AVB_BACKEND_FFMPEG;
    avb_decoder *d = nullptr;
    std::string r = "(open-failed)";
    if (avb_decoder_open(&d, path, &o) == AVB_OK) {
        avb_media_info mi{}; avb_decoder_get_media_info(d, &mi);
        const char *c = video ? mi.video.codec_name : mi.audio.codec_name;
        r = c ? c : "(null)";
    }
    avb_decoder_close(d);
    return r;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <fixture.mp4> <out_prefix>\n", argv[0]);
        return 2;
    }
    const char *fix = argv[1];
    std::string pfx = argv[2];

    if (!avb_backend_is_available(AVB_BACKEND_FFMPEG)) {
        printf("SKIP: FFmpeg backend not built into this library\n");
        return AVB_TEST_SKIP;
    }
    // Confirm the runtime libraries actually load by opening the fixture once.
    {
        avb_decode_options o = avb_decode_options_default();
        o.backend = AVB_BACKEND_FFMPEG;
        avb_decoder *d = nullptr;
        avb_result r = avb_decoder_open(&d, fix, &o);
        avb_decoder_close(d);
        if (r == AVB_ERROR_BACKEND_NOT_AVAILABLE) {
            printf("SKIP: FFmpeg runtime libraries not available\n");
            return AVB_TEST_SKIP;
        }
    }

    printf("HEVC video round-trip:\n");
    std::string hevc = pfx + "_hevc.mp4";
    check(transcode_video(fix, hevc.c_str(), AVB_CODEC_HEVC, AVB_PIXEL_FORMAT_BGRA8), "encode HEVC");
    check(probe_codec(hevc.c_str(), true) == "hevc", "output codec == hevc");

    printf("VP9 video round-trip (webm):\n");
    std::string vp9 = pfx + "_vp9.webm";
    check(transcode_video(fix, vp9.c_str(), AVB_CODEC_VP9, AVB_PIXEL_FORMAT_BGRA8), "encode VP9");
    check(probe_codec(vp9.c_str(), true) == "vp9", "output codec == vp9");

    printf("Opus audio round-trip (48k):\n");
    std::string opus = pfx + "_opus.mp4";
    check(transcode_audio(fix, opus.c_str(), AVB_CODEC_OPUS, 48000), "encode Opus");
    check(probe_codec(opus.c_str(), false) == "opus", "output codec == opus");

    printf("I420 encoder input round-trip:\n");
    std::string i420 = pfx + "_i420.mp4";
    check(transcode_video(fix, i420.c_str(), AVB_CODEC_H264, AVB_PIXEL_FORMAT_I420), "encode from I420 frames");
    check(probe_codec(i420.c_str(), true) == "h264", "output codec == h264");

    printf("\n%s (%d failure%s)\n",
           g_failures == 0 ? "CODEC CHECKS PASSED" : "CODEC CHECKS FAILED",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
