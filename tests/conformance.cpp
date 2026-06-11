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

// Returned to CTest (via SKIP_RETURN_CODE) when the requested backend is not
// compiled into this build, so the run is reported as skipped, not failed.
static const int AVB_TEST_SKIP = 77;

static int dummy_can_decode(const avb_video_stream_info *,
                            const avb_decode_options *) {
    return 0;
}

static avb_result dummy_open(void **, const avb_video_stream_info *,
                             const avb_decode_options *) {
    return AVB_ERROR_OPEN_FAILED;
}

static avb_result dummy_decode_packet(void *, const avb_encoded_packet *,
                                      avb_video_frame *) {
    return AVB_ERROR_AGAIN;
}

static bool has_video_codec(const avb_encoder_capabilities &caps,
                            avb_video_codec codec) {
    for (int i = 0; i < caps.video_codec_count; ++i) {
        if (caps.video_codecs[i] == codec) return true;
    }
    return false;
}

static bool has_audio_codec(const avb_encoder_capabilities &caps,
                            avb_audio_codec codec) {
    for (int i = 0; i < caps.audio_codec_count; ++i) {
        if (caps.audio_codecs[i] == codec) return true;
    }
    return false;
}

static bool has_video_memory(const avb_encoder_capabilities &caps,
                             avb_video_memory_type memory) {
    for (int i = 0; i < caps.video_memory_count; ++i) {
        if (caps.video_memory[i] == memory) return true;
    }
    return false;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <fixture.mp4> [backend]\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];

    // Optional backend name (default "auto"); skip if it isn't in this build.
    avb_backend backend = AVB_BACKEND_AUTO;
    if (argc >= 3) {
        if (avb_backend_from_name(argv[2], &backend) != AVB_OK) {
            fprintf(stderr, "unknown backend '%s'\n", argv[2]);
            return 2;
        }
        if (!avb_backend_is_available(backend)) {
            printf("SKIP: backend '%s' not built into this library\n", argv[2]);
            return AVB_TEST_SKIP;
        }
    }
    printf("Requested backend: %s\n", avb_backend_name(backend));

    // --- Library introspection (backend-independent) ---
    printf("introspection:\n");
    check(avb_version_string() && avb_version_string()[0], "version string non-empty");
    {
        avb_backend rb;
        check(avb_backend_from_name("ffmpeg", &rb) == AVB_OK && rb == AVB_BACKEND_FFMPEG,
              "backend_from_name(ffmpeg) round-trips");
        check(avb_backend_from_name("bogus", &rb) == AVB_ERROR_INVALID_ARGUMENT,
              "backend_from_name(bogus) -> invalid argument");
    }
    {
        const char *n = avb_backend_name(AVB_BACKEND_GSTREAMER);
        check(n && std::strcmp(n, "gstreamer") == 0, "backend_name(gstreamer)");
    }
    {
        avb_video_codec vc;
        avb_audio_codec ac;
        check(std::strcmp(avb_video_codec_name(AVB_VIDEO_CODEC_HEVC), "hevc") == 0,
              "video_codec_name(HEVC)");
        check(avb_video_codec_from_name("vp9", &vc) == AVB_OK &&
              vc == AVB_VIDEO_CODEC_VP9,
              "video_codec_from_name(vp9)");
        check(avb_video_codec_from_name("aac", &vc) == AVB_ERROR_INVALID_ARGUMENT,
              "video_codec_from_name(aac) rejects audio codec");
        check(std::strcmp(avb_audio_codec_name(AVB_AUDIO_CODEC_FLAC), "flac") == 0,
              "audio_codec_name(FLAC)");
        check(avb_audio_codec_from_name("pcm_f32", &ac) == AVB_OK &&
              ac == AVB_AUDIO_CODEC_PCM_F32,
              "audio_codec_from_name(pcm_f32)");
        check(avb_audio_codec_from_name("h264", &ac) == AVB_ERROR_INVALID_ARGUMENT,
              "audio_codec_from_name(h264) rejects video codec");
    }
    check(std::strcmp(avb_result_string(AVB_ERROR_EOF), "AVB_ERROR_EOF") == 0,
          "result_string(EOF)");
    check(std::strcmp(avb_result_string(AVB_ERROR_AGAIN), "AVB_ERROR_AGAIN") == 0,
          "result_string(AGAIN)");
    {
        avb_video_decoder_plugin plugin{};
        plugin.struct_size = sizeof(plugin);
        plugin.name = "dummy";
        plugin.can_decode = dummy_can_decode;
        plugin.open = dummy_open;
        plugin.decode_packet = dummy_decode_packet;
        check(avb_register_video_decoder(&plugin) == AVB_OK,
              "register dummy video decoder");
        check(avb_unregister_video_decoder(&plugin) == AVB_OK,
              "unregister dummy video decoder");
    }
    // A default-constructed decode options must enable both tracks (no footgun).
    {
        avb_decode_options d = avb_decode_options_default();
        check(d.enable_audio == 1 && d.enable_video == 1,
              "decode_options_default enables audio+video");
        check(d.enable_custom_video_decoders == 1,
              "decode_options_default enables custom video decoders");

        d.backend = backend;
        avb_decoder_validation v{};
        check(avb_decoder_validate_options(&d, &v) == AVB_OK,
              "decoder_validate_options runs");
        check(v.ok && v.result == AVB_OK,
              "decoder_validate_options accepts default shape");
        check(v.backend_name && v.message,
              "decoder validation reports names/message");

        d.audio_stream_index = -2;
        check(avb_decoder_validate_options(&d, &v) == AVB_OK &&
              !v.ok && v.result == AVB_ERROR_INVALID_ARGUMENT,
              "decoder_validate_options rejects invalid stream index");

        d = avb_decode_options_default();
        d.backend = backend;
        d.video_memory = AVB_VIDEO_MEMORY_NATIVE;
        d.hardware_policy = AVB_HARDWARE_DISABLED;
        check(avb_decoder_validate_options(&d, &v) == AVB_OK &&
              !v.ok && v.result == AVB_ERROR_INVALID_ARGUMENT,
              "decoder_validate_options rejects native output with hardware disabled");
    }
    {
        avb_encode_options e = avb_encode_options_default();
        e.backend = backend;
        e.video.enable = 1;
        e.video.width = 320;
        e.video.height = 240;
        e.video.frame_rate = 25.0;
        e.video.codec = AVB_VIDEO_CODEC_H264;
        e.video.input_format = AVB_PIXEL_FORMAT_BGRA8;
        e.audio.enable = 1;
        e.audio.sample_rate = 44100;
        e.audio.channels = 1;
        e.audio.codec = AVB_AUDIO_CODEC_AAC;
        avb_encoder_validation v{};
        check(avb_encoder_validate_options(path, &e, &v) == AVB_OK,
              "encoder_validate_options runs");
        check(v.ok && v.result == AVB_OK,
              "encoder_validate_options accepts h264/aac mp4 shape");
        check(v.backend_name && v.container_name && v.video_codec_name && v.audio_codec_name,
              "encoder validation reports names");

        e.audio.codec = AVB_AUDIO_CODEC_OPUS;
        check(avb_encoder_validate_options(path, &e, &v) == AVB_OK &&
              !v.ok && v.result == AVB_ERROR_INVALID_ARGUMENT,
              "encoder_validate_options rejects opus in mp4");
    }
    {
        avb_encoder_capabilities caps{};
        check(avb_encoder_query_capabilities(backend, path, &caps) == AVB_OK,
              "encoder_query_capabilities runs");
        check(caps.result == AVB_OK,
              "encoder_query_capabilities reports usable backend");
        check(caps.backend_name && caps.container_name && caps.message,
              "encoder capabilities reports names/message");
        check(caps.can_encode_video && has_video_codec(caps, AVB_VIDEO_CODEC_H264),
              "encoder capabilities includes h264 for mp4");
        check(caps.can_encode_audio && has_audio_codec(caps, AVB_AUDIO_CODEC_AAC),
              "encoder capabilities includes aac for mp4");
        check(has_video_memory(caps, AVB_VIDEO_MEMORY_CPU),
              "encoder capabilities includes CPU video memory");

        check(avb_encoder_query_capabilities(backend, "out.ogg", &caps) == AVB_OK &&
              caps.result == AVB_OK,
              "encoder capabilities filters ogg container");
        check(caps.can_encode_video == 0 &&
              !has_video_codec(caps, AVB_VIDEO_CODEC_H264),
              "encoder capabilities excludes video for ogg");
        check(!has_audio_codec(caps, AVB_AUDIO_CODEC_AAC),
              "encoder capabilities excludes aac for ogg");
        if (caps.backend == AVB_BACKEND_FFMPEG ||
            caps.backend == AVB_BACKEND_GSTREAMER) {
            check(caps.can_encode_audio &&
                  has_audio_codec(caps, AVB_AUDIO_CODEC_OPUS) &&
                  has_audio_codec(caps, AVB_AUDIO_CODEC_VORBIS),
                  "encoder capabilities includes opus/vorbis for ogg");
        }

        check(avb_encoder_query_capabilities(backend, nullptr, &caps) == AVB_OK &&
              caps.result == AVB_OK &&
              has_video_codec(caps, AVB_VIDEO_CODEC_H264) &&
              has_audio_codec(caps, AVB_AUDIO_CODEC_AAC),
              "encoder capabilities supports broad query without path");
        check(avb_encoder_query_capabilities(backend, path, nullptr) ==
              AVB_ERROR_INVALID_ARGUMENT,
              "encoder_query_capabilities rejects null output");
    }

    avb_decode_options opts{};
    opts.backend            = backend;
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

    printf("probe:\n");
    {
        avb_media_probe probe{};
        check(avb_probe_media(path, &opts, &probe) == AVB_OK,
              "avb_probe_media succeeds");
        check(probe.result == AVB_OK, "probe result == OK");
        check_str(probe.backend_name, info.backend_name, "probe backend matches decoder");
        check_near(probe.duration_sec, info.duration_sec, 0.01,
                   "probe duration matches decoder");
        check(probe.audio.available == info.audio.available,
              "probe audio availability matches decoder");
        check(probe.video.available == info.video.available,
              "probe video availability matches decoder");
        check_str(probe.audio.codec_name, info.audio.codec_name,
                  "probe audio codec matches decoder");
        check_str(probe.video.codec_name, info.video.codec_name,
                  "probe video codec matches decoder");

        avb_decode_options bad = opts;
        bad.enable_audio = 0;
        bad.enable_video = 0;
        check(avb_probe_media(path, &bad, &probe) == AVB_ERROR_INVALID_ARGUMENT &&
              probe.result == AVB_ERROR_INVALID_ARGUMENT &&
              probe.error[0] != '\0',
              "avb_probe_media reports validation error");
    }

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
    // Drain audio on a dedicated audio-only decoder. Bulk-draining one stream to
    // EOF while never pulling the other is not supported on a dual-stream
    // decoder (see avb_decoder_read_* docs), so each stream is decoded alone.
    printf("audio decode:\n");
    {
        avb_decode_options ao = opts;
        ao.enable_video = 0;
        avb_decoder *adec = nullptr;
        check(avb_decoder_open(&adec, path, &ao) == AVB_OK, "open audio-only decoder");

        const int BLOCK = 4096;
        std::vector<float> buf(BLOCK * info.audio.channels);
        double sumsq = 0.0;
        long count = 0;
        int total_audio_frames = 0;
        for (;;) {
            int got = avb_decoder_read_audio_f32(adec, buf.data(), BLOCK, nullptr);
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
        avb_decoder_close(adec);
    }

    // --- Video decode ---
    printf("video decode:\n");
    {
        avb_decode_options vo = opts;
        vo.enable_audio = 0;
        avb_decoder *vdec = nullptr;
        check(avb_decoder_open(&vdec, path, &vo) == AVB_OK, "open video-only decoder");

        int total_video_frames = 0;
        for (;;) {
            avb_video_frame f{};
            avb_result vr = avb_decoder_read_video_frame(vdec, &f);
            if (vr == AVB_ERROR_EOF) break;
            if (vr != AVB_OK) { check(false, "read_video_frame returns OK or EOF"); break; }

            if (total_video_frames == 0) {
                check(f.width == 320 && f.height == 240, "video frame is 320x240");
                check(f.stride >= f.width, "video frame stride >= width");
                check(f.data != nullptr && f.data_size > 0, "video frame has data");
                check(f.data_size >= f.stride * f.height, "data_size covers stride*height");
            }
            avb_decoder_release_video_frame(vdec, &f);
            ++total_video_frames;
        }
        // 3s * 25fps = 75 frames; allow a small tolerance for fps rounding.
        check_near(total_video_frames, 75, 3, "decoded ~75 video frames");
        avb_decoder_close(vdec);
    }

    // --- Seek ---
    printf("seek:\n");
    {
        avb_result sr = avb_decoder_seek(ctx, 1.5, nullptr);
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
        { AVB_PIXEL_FORMAT_I420,  "I420",  3, false },
    };
    for (const auto &fc : cases) {
        avb_decode_options fopts{};
        fopts.backend            = backend;
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
            if (fc.fmt == AVB_PIXEL_FORMAT_I420) {
                // Y full height; Cb and Cr each half width and half height.
                long expect = (long)f.plane_stride[0] * f.height
                            + (long)f.plane_stride[1] * (f.height / 2)
                            + (long)f.plane_stride[2] * (f.height / 2);
                check(f.data_size == expect, "I420: data_size == Y + Cb + Cr planes");
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
        aopts.backend            = backend;
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
                int got = avb_decoder_read_audio_f32(actx, buf.data(), 4096, nullptr);
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
