// Encode round-trip test: decode the fixture, re-encode it to MP4 (H.264 + AAC)
// via the encoder, then decode the result and assert the media survived. Skips
// cleanly (exit 0) on platforms whose encoder backend is not implemented yet.
//
// Usage: avb_roundtrip <fixture.mp4> <output.mp4>

#include <avbridge.h>

#include <cmath>
#include <cstdio>
#include <string>
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

static const int AVB_TEST_SKIP = 77;

static void check_fractional_rate(const char *base_path, avb_backend backend,
                                  double rate) {
    const std::string path = std::string(base_path) + ".fractional.mp4";
    avb_encode_options options = avb_encode_options_default();
    options.backend = backend;
    options.video.hardware_policy = AVB_HARDWARE_DISABLED;
    options.audio.enable = 0;
    options.video.enable = 1;
    options.video.width = 32;
    options.video.height = 32;
    options.video.frame_rate = rate;
    options.video.input_format = AVB_PIXEL_FORMAT_BGRA8;
    avb_encoder *encoder = nullptr;
    const avb_result opened = avb_encoder_open(&encoder, path.c_str(), &options);
    check(opened == AVB_OK, "fractional frame rate encoder opens");
    if (opened != AVB_OK) {
        avb_encoder_close(encoder);
        return;
    }
    std::vector<unsigned char> pixels(32 * 32 * 4, 127);
    avb_video_frame frame{};
    frame.width = frame.height = 32;
    frame.format = AVB_PIXEL_FORMAT_BGRA8;
    frame.data = pixels.data();
    frame.stride = 32 * 4;
    frame.data_size = (int)pixels.size();
    frame.pts_sec = -1.0;
    for (int i = 0; i < 60; ++i) {
        if (avb_encoder_write_video(encoder, &frame, -1.0) != AVB_OK) {
            check(false, "fractional frame rate write succeeds");
            break;
        }
    }
    check(avb_encoder_finish(encoder) == AVB_OK, "fractional frame rate finish succeeds");
    avb_encoder_close(encoder);

    avb_decode_options decode = avb_decode_options_default();
    decode.backend = backend;
    decode.enable_audio = 0;
    decode.hardware_policy = AVB_HARDWARE_DISABLED;
    avb_decoder *decoder = nullptr;
    if (avb_decoder_open(&decoder, path.c_str(), &decode) != AVB_OK) {
        check(false, "fractional frame rate output opens");
        avb_decoder_close(decoder);
        return;
    }
    avb_media_info info{};
    avb_decoder_get_media_info(decoder, &info);
    check_near(info.video.frame_rate, rate, 0.0001, "fractional frame rate is preserved");
    int count = 0;
    double first_pts = 0.0;
    double max_pts_error = 0.0;
    avb_result result;
    while ((result = avb_decoder_read_video_frame(decoder, &frame)) == AVB_OK) {
        if (count == 0) first_pts = frame.pts_sec;
        max_pts_error = std::fmax(max_pts_error,
            std::fabs(frame.pts_sec - first_pts - count / rate));
        ++count;
        avb_decoder_release_video_frame(decoder, &frame);
    }
    check(result == AVB_ERROR_EOF && count == 60, "all fractional-rate frames decode");
    check_near(max_pts_error, 0.0, 0.000002, "derived timestamps preserve fractional rate");
    avb_decoder_close(decoder);
    std::remove(path.c_str());
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <fixture.mp4> <output.mp4> [backend]\n", argv[0]);
        return 2;
    }
    const char *in_path = argv[1];
    const char *out_path = argv[2];

    avb_backend backend = AVB_BACKEND_AUTO;
    if (argc >= 4) {
        if (avb_backend_from_name(argv[3], &backend) != AVB_OK) {
            fprintf(stderr, "unknown backend '%s'\n", argv[3]);
            return 2;
        }
        if (!avb_backend_is_available(backend)) {
            printf("SKIP: backend '%s' not built into this library\n", argv[3]);
            return AVB_TEST_SKIP;
        }
    }

    // --- Decode source ---
    avb_decode_options dopts = avb_decode_options_default();
    dopts.backend      = backend;
    dopts.video_format = AVB_PIXEL_FORMAT_BGRA8;

    avb_decoder *dec = nullptr;
    if (avb_decoder_open(&dec, in_path, &dopts) != AVB_OK) {
        fprintf(stderr, "open fixture failed: %s\n",
                avb_decoder_get_last_error(dec) ? avb_decoder_get_last_error(dec) : "unknown");
        avb_decoder_close(dec);
        return 1;
    }
    avb_media_info src{};
    avb_decoder_get_media_info(dec, &src);

    // --- Open encoder (skip test if this backend has no encoder) ---
    avb_encode_options eopts = avb_encode_options_default();
    eopts.backend            = backend;
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
        avb_decoder_close(dec);
        return 0;
    }
    if (eres != AVB_OK) {
        fprintf(stderr, "encoder open failed: %s\n",
                avb_encoder_get_last_error(enc) ? avb_encoder_get_last_error(enc) : "unknown");
        avb_encoder_close(enc);
        avb_decoder_close(dec);
        return 1;
    }

    // --- Transcode, interleaving audio with video by PTS ---
    int vframes = 0;
    long aframes = 0;
    double audio_pts = 0.0;
    std::vector<float> pcm(1024 * src.audio.channels);

    avb_video_frame f{};
    bool have = (avb_decoder_read_video_frame(dec, &f) == AVB_OK);
    while (have) {
        if (avb_encoder_write_video(enc, &f, f.pts_sec) != AVB_OK) {
            check(false, "write_video succeeds");
            avb_decoder_release_video_frame(dec, &f);
            break;
        }
        avb_decoder_release_video_frame(dec, &f);
        vframes++;
        have = (avb_decoder_read_video_frame(dec, &f) == AVB_OK);
        double target = have ? f.pts_sec : 1e9;
        while (audio_pts <= target) {
            int got = avb_decoder_read_audio_f32(dec, pcm.data(), 1024, nullptr);
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
    avb_decoder_close(dec);

    printf("encoded %d video / %ld audio frames\n", vframes, aframes);

    // --- Re-decode the encoded output and verify it survived the round-trip ---
    avb_decoder *re = nullptr;
    if (avb_decoder_open(&re, out_path, &dopts) != AVB_OK) {
        fprintf(stderr, "re-open encoded output failed: %s\n",
                avb_decoder_get_last_error(re) ? avb_decoder_get_last_error(re) : "unknown");
        avb_decoder_close(re);
        return 1;
    }
    avb_media_info out{};
    avb_decoder_get_media_info(re, &out);

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
    check(avb_decoder_read_video_frame(re, &rf) == AVB_OK, "output video frame decodes");
    avb_decoder_release_video_frame(re, &rf);
    avb_decoder_close(re);

    if (backend == AVB_BACKEND_FFMPEG || backend == AVB_BACKEND_GSTREAMER) {
        for (double rate : {30000.0 / 1001.0, 24000.0 / 1001.0, 12.5})
            check_fractional_rate(out_path, backend, rate);
    }

    printf("\n%s (%d failure%s)\n",
           g_failures == 0 ? "ROUND-TRIP PASSED" : "ROUND-TRIP FAILED",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
