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

    avb_decode_options opts{};
    opts.backend            = AVB_BACKEND_AUTO;
    opts.audio_stream_index = -1;
    opts.video_stream_index = -1;
    opts.enable_audio       = 1;
    opts.enable_video       = 1;

    avb_decoder *ctx = nullptr;
    avb_result res = avb_decoder_open(&ctx, path, &opts);
    if (res != AVB_OK) {
        fprintf(stderr, "avb_decoder_open failed (%d): %s\n", res,
                avb_decoder_get_last_error(ctx) ? avb_decoder_get_last_error(ctx) : "unknown");
        avb_decoder_close(ctx);
        return 1;
    }

    avb_media_info info{};
    res = avb_decoder_get_media_info(ctx, &info);
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
            int got = avb_decoder_read_audio_f32(ctx, buf.data(), BLOCK);
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
            avb_result vr = avb_decoder_read_video_frame(ctx, &f);
            if (vr == AVB_ERROR_EOF) break;
            if (vr != AVB_OK) { check(false, "read_video_frame returns OK or EOF"); break; }

            if (total_video_frames == 0) {
                check(f.width == 320 && f.height == 240, "video frame is 320x240");
                check(f.stride >= f.width, "video frame stride >= width");
                check(f.data != nullptr && f.data_size > 0, "video frame has data");
                check(f.data_size >= f.stride * f.height, "data_size covers stride*height");
            }
            avb_decoder_release_video_frame(ctx, &f);
            ++total_video_frames;
        }
        // 3s * 25fps = 75 frames; allow a small tolerance for fps rounding.
        check_near(total_video_frames, 75, 3, "decoded ~75 video frames");
    }

    // --- Seek ---
    printf("seek:\n");
    {
        avb_result sr = avb_decoder_seek(ctx, 1.5);
        check(sr == AVB_OK, "seek(1.5) succeeds");
        avb_video_frame f{};
        avb_result vr = avb_decoder_read_video_frame(ctx, &f);
        check(vr == AVB_OK, "read_video_frame after seek succeeds");
        if (vr == AVB_OK) {
            check_near(f.pts_sec, 1.5, 0.2, "first frame pts after seek ~1.5s");
            avb_decoder_release_video_frame(ctx, &f);
        }
    }

    avb_decoder_close(ctx);

    // --- Pixel format selection ---
    // Re-open video-only in each requested format and validate the frame layout.
    printf("pixel formats:\n");
    // BGRA8 and RGBA8 are mandatory for every backend; NV12 is optional (some
    // backends may not implement planar output) and only validated where the
    // open succeeds.
    struct FmtCase { avb_pixel_format fmt; const char *name; int planes; bool mandatory; };
    const FmtCase cases[] = {
        { AVB_PIXEL_FORMAT_BGRA8, "BGRA8", 1, true  },
        { AVB_PIXEL_FORMAT_RGBA8, "RGBA8", 1, true  },
        { AVB_PIXEL_FORMAT_NV12,  "NV12",  2, false },
    };
    for (const auto &fc : cases) {
        avb_decode_options fopts{};
        fopts.backend            = AVB_BACKEND_AUTO;
        fopts.audio_stream_index = -1;
        fopts.video_stream_index = -1;
        fopts.enable_audio       = 0;
        fopts.enable_video       = 1;
        fopts.video_format       = fc.fmt;

        avb_decoder *fctx = nullptr;
        char msg[128];
        if (avb_decoder_open(&fctx, path, &fopts) != AVB_OK) {
            if (fc.mandatory) {
                snprintf(msg, sizeof(msg), "open with %s succeeds", fc.name);
                check(false, msg);
            } else {
                printf("  [SKIP] %s not supported by this backend\n", fc.name);
            }
            avb_decoder_close(fctx);
            continue;
        }
        avb_video_frame f{};
        avb_result vr = avb_decoder_read_video_frame(fctx, &f);
        snprintf(msg, sizeof(msg), "%s: decode first frame", fc.name);
        check(vr == AVB_OK, msg);
        if (vr == AVB_OK) {
            snprintf(msg, sizeof(msg), "%s: format reported back", fc.name);
            check(f.format == fc.fmt, msg);
            snprintf(msg, sizeof(msg), "%s: plane_count == %d", fc.name, fc.planes);
            check(f.plane_count == fc.planes, msg);
            snprintf(msg, sizeof(msg), "%s: data aliases plane 0", fc.name);
            check(f.data == f.plane_data[0] && f.stride == f.plane_stride[0], msg);
            for (int p = 0; p < f.plane_count; ++p) {
                snprintf(msg, sizeof(msg), "%s: plane %d has data and stride", fc.name, p);
                check(f.plane_data[p] != nullptr && f.plane_stride[p] > 0, msg);
            }
            if (fc.fmt == AVB_PIXEL_FORMAT_NV12) {
                // Y plane is full height, CbCr plane is half height.
                long expect = (long)f.plane_stride[0] * f.height
                            + (long)f.plane_stride[1] * (f.height / 2);
                check(f.data_size == expect, "NV12: data_size == Y + CbCr planes");
            }
            avb_decoder_release_video_frame(fctx, &f);
        }
        avb_decoder_close(fctx);
    }

    // --- Audio output format control ---
    // Request a non-source rate and channel count; the backend must report the
    // effective values and produce interleaved float at that layout.
    printf("audio format control:\n");
    {
        avb_decode_options aopts{};
        aopts.backend            = AVB_BACKEND_AUTO;
        aopts.audio_stream_index = -1;
        aopts.video_stream_index = -1;
        aopts.enable_audio       = 1;
        aopts.enable_video       = 0;
        aopts.audio_sample_rate  = 22050;
        aopts.audio_channels     = 2;

        avb_decoder *actx = nullptr;
        if (avb_decoder_open(&actx, path, &aopts) != AVB_OK) {
            check(false, "open with audio override succeeds");
        } else {
            avb_media_info ai{};
            avb_decoder_get_media_info(actx, &ai);
            check(ai.audio.sample_rate == 22050, "audio resampled to 22050 Hz");
            check(ai.audio.channels == 2, "audio remixed to 2 channels");

            std::vector<float> buf(4096 * 2);
            long frames = 0;
            for (;;) {
                int got = avb_decoder_read_audio_f32(actx, buf.data(), 4096);
                if (got <= 0) break;
                frames += got;
            }
            double seconds = (double)frames / 22050;
            check_near(seconds, 3.0, 0.2, "resampled audio duration ~3.0s");
        }
        avb_decoder_close(actx);
    }

    printf("\n%s (%d failure%s)\n",
           g_failures == 0 ? "ALL CHECKS PASSED" : "CHECKS FAILED",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
