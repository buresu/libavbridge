// Encode round-trip test: decode the fixture, re-encode it to MP4 (H.264 + AAC)
// via the encoder, then decode the result and assert the media survived. Skips
// cleanly (exit 0) on platforms whose encoder backend is not implemented yet.
//
// Usage: avb_roundtrip <fixture.mp4> <output.mp4>

#include <avbridge.h>

#include <cmath>
#include <cstdio>
#include <vector>

static int g_failures = 0;

static void check(bool cond, const char *what) {
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_failures;
}
static void check_near(double got, double want, double tol, const char *what) {
    bool ok = std::fabs(got - want) <= tol;
    printf("  [%s] %s (got %.3f, want %.3f +/- %.3f)\n",
           ok ? "PASS" : "FAIL", what, got, want, tol);
    if (!ok) ++g_failures;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <fixture.mp4> <output.mp4>\n", argv[0]);
        return 2;
    }
    const char *in_path = argv[1];
    const char *out_path = argv[2];

    // --- Decode source ---
    avb_open_options dopts{};
    dopts.backend      = AVB_BACKEND_AUTO;
    dopts.audio_stream_index = -1;
    dopts.video_stream_index = -1;
    dopts.enable_audio = 1;
    dopts.enable_video = 1;
    dopts.video_format = AVB_PIXEL_FORMAT_BGRA8;

    avb_context *dec = nullptr;
    if (avb_open_file(&dec, in_path, &dopts) != AVB_OK) {
        fprintf(stderr, "open fixture failed: %s\n",
                avb_get_last_error(dec) ? avb_get_last_error(dec) : "unknown");
        avb_close(dec);
        return 1;
    }
    avb_media_info src{};
    avb_get_media_info(dec, &src);

    // --- Open encoder (skip test if this backend has no encoder) ---
    avb_encode_options eopts{};
    eopts.backend            = AVB_BACKEND_AUTO;
    eopts.video.enable       = 1;
    eopts.video.width        = src.video.width;
    eopts.video.height       = src.video.height;
    eopts.video.frame_rate   = src.video.frame_rate;
    eopts.video.input_format = AVB_PIXEL_FORMAT_BGRA8;
    eopts.video.bitrate      = 2000000;
    eopts.audio.enable       = 1;
    eopts.audio.sample_rate  = src.audio.sample_rate;
    eopts.audio.channels     = src.audio.channels;

    avb_encoder *enc = nullptr;
    avb_result eres = avb_encoder_open(&enc, out_path, &eopts);
    if (eres == AVB_ERROR_BACKEND_NOT_AVAILABLE) {
        printf("SKIP: encoder backend not available on this platform\n");
        avb_encoder_close(enc);
        avb_close(dec);
        return 0;
    }
    if (eres != AVB_OK) {
        fprintf(stderr, "encoder open failed: %s\n",
                avb_encoder_get_last_error(enc) ? avb_encoder_get_last_error(enc) : "unknown");
        avb_encoder_close(enc);
        avb_close(dec);
        return 1;
    }

    // --- Transcode, interleaving audio with video by PTS ---
    int vframes = 0;
    long aframes = 0;
    double audio_pts = 0.0;
    std::vector<float> pcm(1024 * src.audio.channels);

    avb_video_frame f{};
    bool have = (avb_read_video_frame(dec, &f) == AVB_OK);
    while (have) {
        if (avb_encoder_write_video(enc, &f, f.pts_sec) != AVB_OK) {
            check(false, "write_video succeeds");
            avb_release_video_frame(dec, &f);
            break;
        }
        avb_release_video_frame(dec, &f);
        vframes++;
        have = (avb_read_video_frame(dec, &f) == AVB_OK);
        double target = have ? f.pts_sec : 1e9;
        while (audio_pts <= target) {
            int got = avb_read_audio_f32(dec, pcm.data(), 1024);
            if (got <= 0) break;
            if (avb_encoder_write_audio_f32(enc, pcm.data(), got) != AVB_OK) {
                check(false, "write_audio succeeds");
                break;
            }
            aframes += got;
            audio_pts += (double)got / src.audio.sample_rate;
        }
    }

    check(avb_encoder_finish(enc) == AVB_OK, "encoder finish succeeds");
    avb_encoder_close(enc);
    avb_close(dec);

    printf("encoded %d video / %ld audio frames\n", vframes, aframes);

    // --- Re-decode the encoded output and verify it survived the round-trip ---
    avb_context *re = nullptr;
    if (avb_open_file(&re, out_path, &dopts) != AVB_OK) {
        fprintf(stderr, "re-open encoded output failed: %s\n",
                avb_get_last_error(re) ? avb_get_last_error(re) : "unknown");
        avb_close(re);
        return 1;
    }
    avb_media_info out{};
    avb_get_media_info(re, &out);

    printf("round-trip media_info:\n");
    check(out.video.available, "output has video");
    check(out.video.width == src.video.width, "output video width preserved");
    check(out.video.height == src.video.height, "output video height preserved");
    check(out.audio.available, "output has audio");
    check(out.audio.sample_rate == src.audio.sample_rate, "output sample_rate preserved");
    check(out.audio.channels == src.audio.channels, "output channels preserved");
    check_near(out.duration_sec, src.duration_sec, 0.3, "output duration preserved");

    // The output must be decodable: pull at least one video frame back out.
    avb_video_frame rf{};
    check(avb_read_video_frame(re, &rf) == AVB_OK, "output video frame decodes");
    avb_release_video_frame(re, &rf);
    avb_close(re);

    printf("\n%s (%d failure%s)\n",
           g_failures == 0 ? "ROUND-TRIP PASSED" : "ROUND-TRIP FAILED",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
