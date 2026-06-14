// Codec / pixel-format coverage. FFmpeg runs the broad set because it has the
// widest codec support; GStreamer runs the web video codecs covered by plugins.
// Each output is re-opened and checked for the expected codec and decodability.
// Skips cleanly if the selected backend is not built in or its runtime libraries
// are unavailable.
//
// Usage: avb_codecs <fixture.mp4> <out_prefix> [backend] [video-only|audio-only]

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
static bool transcode_video(const char *in, const char *out, avb_backend backend, avb_video_codec vc,
                            avb_pixel_format infmt) {
    avb_decode_options dop = avb_decode_options_default();
    dop.backend = backend;
    dop.enable_audio = 0; dop.enable_video = 1; dop.video_format = infmt;
    avb_decoder *d = nullptr;
    if (avb_decoder_open(&d, in, &dop) != AVB_OK) { avb_decoder_close(d); return false; }
    avb_media_info mi{}; avb_decoder_get_media_info(d, &mi);

    avb_encode_options eo = avb_encode_options_default();
    eo.backend = backend;
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

static bool transcode_audio(const char *in, const char *out, avb_backend backend,
                            avb_audio_codec ac, int rate) {
    avb_decode_options dop = avb_decode_options_default();
    dop.backend = backend;
    dop.enable_audio = 1; dop.enable_video = 0;
    dop.audio_sample_rate = rate; dop.audio_channels = 2;
    avb_decoder *d = nullptr;
    if (avb_decoder_open(&d, in, &dop) != AVB_OK) { avb_decoder_close(d); return false; }
    avb_media_info mi{}; avb_decoder_get_media_info(d, &mi);

    avb_encode_options eo = avb_encode_options_default();
    eo.backend = backend;
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

static std::string probe_codec(const char *path, avb_backend backend, bool video);

static void check_audio_codec(const char *label, const char *in, const std::string &out,
                              avb_backend backend, avb_audio_codec codec, int rate,
                              const char *expected_codec) {
    printf("%s audio round-trip:\n", label);
    check(transcode_audio(in, out.c_str(), backend, codec, rate), "encode audio");
    std::string actual = probe_codec(out.c_str(), backend, false);
    if (actual != expected_codec) {
        printf("    expected codec '%s', got '%s'\n", expected_codec, actual.c_str());
    }
    check(actual == expected_codec, "output codec matches");
}

// Re-open `path` with the selected backend and return the audio/video codec name.
static std::string probe_codec(const char *path, avb_backend backend, bool video) {
    avb_decode_options o = avb_decode_options_default();
    o.backend = backend;
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
        fprintf(stderr, "Usage: %s <fixture.mp4> <out_prefix> [backend] [video-only|audio-only]\n", argv[0]);
        return 2;
    }
    const char *fix = argv[1];
    std::string pfx = argv[2];
    avb_backend backend = AVB_BACKEND_FFMPEG;
    const char *backend_name = "ffmpeg";
    if (argc >= 4) {
        backend_name = argv[3];
        if (avb_backend_from_name(backend_name, &backend) != AVB_OK) {
            fprintf(stderr, "unknown backend '%s'\n", backend_name);
            return 2;
        }
    }
    bool video_only = argc >= 5 && std::strcmp(argv[4], "video-only") == 0;
    bool audio_only = argc >= 5 && std::strcmp(argv[4], "audio-only") == 0;

    if (!avb_backend_is_available(backend)) {
        printf("SKIP: backend '%s' not built into this library\n", backend_name);
        return AVB_TEST_SKIP;
    }
    // Confirm the runtime libraries actually load by opening the fixture once.
    {
        avb_decode_options o = avb_decode_options_default();
        o.backend = backend;
        avb_decoder *d = nullptr;
        avb_result r = avb_decoder_open(&d, fix, &o);
        avb_decoder_close(d);
        if (r == AVB_ERROR_BACKEND_NOT_AVAILABLE) {
            printf("SKIP: backend '%s' runtime libraries not available\n", backend_name);
            return AVB_TEST_SKIP;
        }
    }

    avb_pixel_format web_video_input = video_only
        ? AVB_PIXEL_FORMAT_I420
        : AVB_PIXEL_FORMAT_BGRA8;

    if (!video_only && !audio_only) {
        printf("HEVC video round-trip:\n");
        std::string hevc = pfx + "_hevc.mp4";
        check(transcode_video(fix, hevc.c_str(), backend, AVB_VIDEO_CODEC_HEVC, AVB_PIXEL_FORMAT_BGRA8), "encode HEVC");
        check(probe_codec(hevc.c_str(), backend, true) == "hevc", "output codec == hevc");
    }

    if (!audio_only) {
        printf("VP8 video round-trip (webm):\n");
        std::string vp8 = pfx + "_vp8.webm";
        check(transcode_video(fix, vp8.c_str(), backend, AVB_VIDEO_CODEC_VP8, web_video_input), "encode VP8");
        check(probe_codec(vp8.c_str(), backend, true) == "vp8", "output codec == vp8");

        printf("VP9 video round-trip (webm):\n");
        std::string vp9 = pfx + "_vp9.webm";
        check(transcode_video(fix, vp9.c_str(), backend, AVB_VIDEO_CODEC_VP9, web_video_input), "encode VP9");
        check(probe_codec(vp9.c_str(), backend, true) == "vp9", "output codec == vp9");

        printf("AV1 video round-trip (mkv):\n");
        std::string av1 = pfx + "_av1.mkv";
        check(transcode_video(fix, av1.c_str(), backend, AVB_VIDEO_CODEC_AV1, web_video_input), "encode AV1");
        check(probe_codec(av1.c_str(), backend, true) == "av1", "output codec == av1");
    }

    if (!video_only) {
        const char *mp3_name = backend == AVB_BACKEND_GSTREAMER ? "mp3" : "mp3float";
        const char *pcm_s16_name = backend == AVB_BACKEND_GSTREAMER ? "wav" : "pcm_s16le";
        const char *pcm_f32_name = backend == AVB_BACKEND_GSTREAMER ? "wav" : "pcm_f32le";

        check_audio_codec("Opus (48k)", fix, pfx + "_opus.ogg", backend,
                          AVB_AUDIO_CODEC_OPUS, 48000, "opus");
        check_audio_codec("MP3", fix, pfx + "_mp3.mp3", backend,
                          AVB_AUDIO_CODEC_MP3, 44100, mp3_name);
        check_audio_codec("FLAC", fix, pfx + "_flac.flac", backend,
                          AVB_AUDIO_CODEC_FLAC, 44100, "flac");
        check_audio_codec("Vorbis", fix, pfx + "_vorbis.ogg", backend,
                          AVB_AUDIO_CODEC_VORBIS, 44100, "vorbis");
        check_audio_codec("PCM S16", fix, pfx + "_pcm_s16.wav", backend,
                          AVB_AUDIO_CODEC_PCM_S16, 44100, pcm_s16_name);
        check_audio_codec("PCM F32", fix, pfx + "_pcm_f32.wav", backend,
                          AVB_AUDIO_CODEC_PCM_F32, 44100, pcm_f32_name);
    }

    if (!video_only && !audio_only) {
        printf("I420 encoder input round-trip:\n");
        std::string i420 = pfx + "_i420.mp4";
        check(transcode_video(fix, i420.c_str(), backend, AVB_VIDEO_CODEC_H264, AVB_PIXEL_FORMAT_I420), "encode from I420 frames");
        check(probe_codec(i420.c_str(), backend, true) == "h264", "output codec == h264");
    }

    printf("\n%s (%d failure%s)\n",
           g_failures == 0 ? "CODEC CHECKS PASSED" : "CODEC CHECKS FAILED",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
