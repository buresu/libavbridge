// Cross-backend conformance test.
//
// Runs the public avbridge API against a known fixture and asserts that the
// active backend (whichever one the platform resolves to) reports consistent
// media info and decodes a sane amount of audio/video. The expectations encode
// the *contract* every backend must satisfy, so the same binary validates the
// FFmpeg, Media Foundation and AVFoundation backends on their respective
// platforms.
//
// Usage: avb_conformance <fixture.mp4>
// The fixture is expected to be:
//   video: 320x240, ~25 fps, ~3.0 s, codec "h264"
//   audio: 44100 Hz, 1 channel, ~3.0 s, codec "aac"
// (see tests/CMakeLists.txt, which generates it with ffmpeg).

#include <avbridge.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;

static void check(bool cond, const char *what) {
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_failures;
}

// Floating-point check with an absolute tolerance, with a descriptive message.
static void check_near(double got, double want, double tol, const char *what) {
    bool ok = std::fabs(got - want) <= tol;
    printf("  [%s] %s (got %.3f, want %.3f +/- %.3f)\n",
           ok ? "PASS" : "FAIL", what, got, want, tol);
    if (!ok) ++g_failures;
}

static void check_str(const char *got, const char *want, const char *what) {
    bool ok = got && std::strcmp(got, want) == 0;
    printf("  [%s] %s (got \"%s\", want \"%s\")\n",
           ok ? "PASS" : "FAIL", what, got ? got : "(null)", want);
    if (!ok) ++g_failures;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <fixture.mp4>\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];

    avb_open_options opts{};
    opts.backend            = AVB_BACKEND_AUTO;
    opts.audio_stream_index = -1;
    opts.video_stream_index = -1;
    opts.enable_audio       = 1;
    opts.enable_video       = 1;

    avb_context *ctx = nullptr;
    avb_result res = avb_open_file(&ctx, path, &opts);
    if (res != AVB_OK) {
        fprintf(stderr, "avb_open_file failed (%d): %s\n", res,
                avb_get_last_error(ctx) ? avb_get_last_error(ctx) : "unknown");
        avb_close(ctx);
        return 1;
    }

    avb_media_info info{};
    res = avb_get_media_info(ctx, &info);
    check(res == AVB_OK, "get_media_info succeeds");

    printf("Backend: %s\n", info.backend_name ? info.backend_name : "(null)");

    printf("media_info:\n");
    check_near(info.duration_sec, 3.0, 0.2, "container duration ~3.0s");

    // --- Audio contract ---
    check(info.audio.available, "audio stream available");
    check(info.audio.sample_rate == 44100, "audio sample_rate == 44100");
    check(info.audio.channels == 1, "audio channels == 1");
    check_str(info.audio.codec_name, "aac", "audio codec_name == aac (source codec)");

    // --- Video contract ---
    check(info.video.available, "video stream available");
    check(info.video.width == 320, "video width == 320");
    check(info.video.height == 240, "video height == 240");
    check_near(info.video.frame_rate, 25.0, 1.0, "video frame_rate ~25fps");
    check_str(info.video.codec_name, "h264", "video codec_name == h264 (source codec)");

    // --- Audio decode ---
    printf("audio decode:\n");
    int total_audio_frames = 0;
    {
        const int BLOCK = 4096;
        std::vector<float> buf(BLOCK * info.audio.channels);
        double sumsq = 0.0;
        long count = 0;
        for (;;) {
            int got = avb_read_audio_f32(ctx, buf.data(), BLOCK);
            if (got <= 0) break;
            for (int i = 0; i < got * info.audio.channels; ++i) {
                sumsq += (double)buf[i] * buf[i];
            }
            count += (long)got * info.audio.channels;
            total_audio_frames += got;
        }
        double seconds = (double)total_audio_frames / info.audio.sample_rate;
        check_near(seconds, 3.0, 0.2, "decoded audio duration ~3.0s");
        double rms = count > 0 ? std::sqrt(sumsq / count) : 0.0;
        check(rms > 0.001, "decoded audio is non-silent");
    }

    // --- Video decode ---
    printf("video decode:\n");
    int total_video_frames = 0;
    {
        for (;;) {
            avb_video_frame f{};
            avb_result vr = avb_read_video_frame(ctx, &f);
            if (vr == AVB_ERROR_EOF) break;
            if (vr != AVB_OK) { check(false, "read_video_frame returns OK or EOF"); break; }

            if (total_video_frames == 0) {
                check(f.width == 320 && f.height == 240, "video frame is 320x240");
                check(f.stride >= f.width, "video frame stride >= width");
                check(f.data != nullptr && f.data_size > 0, "video frame has data");
                check(f.data_size >= f.stride * f.height, "data_size covers stride*height");
            }
            avb_release_video_frame(ctx, &f);
            ++total_video_frames;
        }
        // 3s * 25fps = 75 frames; allow a small tolerance for fps rounding.
        check_near(total_video_frames, 75, 3, "decoded ~75 video frames");
    }

    // --- Seek ---
    printf("seek:\n");
    {
        avb_result sr = avb_seek(ctx, 1.5);
        check(sr == AVB_OK, "seek(1.5) succeeds");
        avb_video_frame f{};
        avb_result vr = avb_read_video_frame(ctx, &f);
        check(vr == AVB_OK, "read_video_frame after seek succeeds");
        if (vr == AVB_OK) {
            check_near(f.pts_sec, 1.5, 0.2, "first frame pts after seek ~1.5s");
            avb_release_video_frame(ctx, &f);
        }
    }

    avb_close(ctx);

    printf("\n%s (%d failure%s)\n",
           g_failures == 0 ? "ALL CHECKS PASSED" : "CHECKS FAILED",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
