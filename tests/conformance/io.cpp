// Custom-I/O and timing API coverage: avb_decoder_open_memory, audio block
// timestamps (out_first_pts), seek landed time, and audio EOF distinction.
// Runs against a chosen backend (default auto); skips if it isn't built in.
//
// Usage: avb_io <fixture.mp4> [backend]

#include <avbridge.h>

#include <algorithm>
#include <cmath>
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
static void check_near(double got, double want, double tol, const char *what) {
    bool ok = std::fabs(got - want) <= tol;
    printf("  [%s] %s (got %.3f, want %.3f +/- %.3f)\n",
           ok ? "PASS" : "FAIL", what, got, want, tol);
    if (!ok) ++g_failures;
}

static std::vector<unsigned char> read_file(const char *path) {
    std::vector<unsigned char> v;
    FILE *f = std::fopen(path, "rb");
    if (!f) return v;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) { v.resize((size_t)n); if (std::fread(v.data(), 1, (size_t)n, f) != (size_t)n) v.clear(); }
    std::fclose(f);
    return v;
}

struct FailingIo {
    const std::vector<unsigned char> &bytes;
    long long pos = 0;
    bool fail = false;
    int failures = 0;
};
static int failing_read(void *user, unsigned char *dst, int size) {
    auto &io = *static_cast<FailingIo *>(user);
    if (io.fail) { ++io.failures; return -1; }
    // Short reads keep avformat's AVIO cache from swallowing this whole small
    // fixture during probing, so reads after open reach the callback too.
    size_t count = std::min((size_t)std::min(size, 1024), io.bytes.size() - (size_t)io.pos);
    std::memcpy(dst, io.bytes.data() + io.pos, count);
    io.pos += (long long)count;
    return (int)count;
}
static long long failing_seek(void *user, long long offset, int whence) {
    auto &io = *static_cast<FailingIo *>(user);
    long long pos = offset + (whence == SEEK_CUR ? io.pos :
                              whence == SEEK_END ? (long long)io.bytes.size() : 0);
    if (pos < 0 || pos > (long long)io.bytes.size()) return -1;
    return io.pos = pos;
}
static long long failing_size(void *user) {
    return (long long)static_cast<FailingIo *>(user)->bytes.size();
}

static void check_read_errors(const std::vector<unsigned char> &bytes, bool audio) {
    FailingIo io{bytes};
    avb_io_callbacks callbacks{};
    callbacks.read = failing_read;
    callbacks.seek = failing_seek;
    callbacks.size = failing_size;
    avb_decode_options options = avb_decode_options_default();
    options.backend = AVB_BACKEND_FFMPEG;
    options.enable_audio = audio;
    options.enable_video = !audio;
    options.hardware_policy = AVB_HARDWARE_DISABLED;
    avb_decoder *decoder = nullptr;
    avb_result opened = avb_decoder_open_io(&decoder, &callbacks, &io, &options);
    check(opened == AVB_OK, "failure-injection input opens");
    if (opened != AVB_OK) { avb_decoder_close(decoder); return; }
    io.fail = true; // Opening/probing succeeded; the subsequent I/O fails.
    avb_result result = AVB_OK;
    std::vector<float> samples(4096 * 2);
    for (int i = 0; i < 1000; ++i) {
        if (audio) {
            if (avb_decoder_read_audio_f32(decoder, samples.data(), 4096, nullptr) == 0) break;
        } else {
            avb_video_frame frame{};
            result = avb_decoder_read_video_frame(decoder, &frame);
            if (result != AVB_OK) break;
            avb_decoder_release_video_frame(decoder, &frame);
        }
    }
    check(io.failures > 0, "a read callback actually fails after open");
    if (audio) {
        check(avb_decoder_audio_at_eof(decoder) == 0, "audio I/O failure is not EOF");
        check(avb_decoder_read_audio_f32(decoder, samples.data(), 4096, nullptr) == 0,
              "audio failure remains terminal until seek");
        check(avb_decoder_audio_at_eof(decoder) == 0, "repeated failed audio read is not EOF");
    } else {
        check(result == AVB_ERROR_DECODE_FAILED, "video I/O failure returns DECODE_FAILED");
        avb_video_frame frame{};
        check(avb_decoder_read_video_frame(decoder, &frame) == AVB_ERROR_DECODE_FAILED,
              "repeated failed video read remains an error");
    }
    const char *error = avb_decoder_get_last_error(decoder);
    check(error && *error, "I/O failure has a diagnostic");
    io.fail = false;
    check(avb_decoder_seek(decoder, 0.0, nullptr) == AVB_OK, "seek recovers after I/O resumes");
    check(avb_decoder_get_last_error(decoder) == nullptr, "successful seek clears the read error");
    for (int i = 0; i < 1000; ++i) {
        if (audio) {
            if (avb_decoder_read_audio_f32(decoder, samples.data(), 4096, nullptr) == 0) break;
        } else {
            avb_video_frame frame{};
            result = avb_decoder_read_video_frame(decoder, &frame);
            if (result != AVB_OK) break;
            avb_decoder_release_video_frame(decoder, &frame);
        }
    }
    check(audio ? avb_decoder_audio_at_eof(decoder) == 1 : result == AVB_ERROR_EOF,
          "recovered input ends with clean EOF");
    avb_decoder_close(decoder);
}

int main(int argc, char *argv[]) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <fixture.mp4> [backend]\n", argv[0]); return 2; }
    const char *path = argv[1];

    avb_backend backend = AVB_BACKEND_AUTO;
    if (argc >= 3) {
        if (avb_backend_from_name(argv[2], &backend) != AVB_OK) { fprintf(stderr, "bad backend\n"); return 2; }
        if (!avb_backend_is_available(backend)) {
            printf("SKIP: backend '%s' not built in\n", argv[2]);
            return AVB_TEST_SKIP;
        }
    }

    std::vector<unsigned char> bytes = read_file(path);
    if (bytes.empty()) { fprintf(stderr, "cannot read fixture\n"); return 1; }

    // --- open_memory: decode straight from the in-memory buffer ---
    printf("open_memory:\n");
    avb_decode_options o = avb_decode_options_default();
    o.backend = backend;
    o.video_format = AVB_PIXEL_FORMAT_BGRA8;

    avb_decoder *dec = nullptr;
    avb_result r = avb_decoder_open_memory(&dec, bytes.data(), bytes.size(), &o);
    if (r == AVB_ERROR_BACKEND_NOT_AVAILABLE) {
        printf("SKIP: backend runtime not available\n");
        avb_decoder_close(dec);
        return AVB_TEST_SKIP;
    }
    check(r == AVB_OK, "open_memory succeeds");
    if (r != AVB_OK) {
        fprintf(stderr, "open_memory: %s\n", avb_decoder_get_last_error(dec));
        avb_decoder_close(dec);
        return 1;
    }

    avb_media_info mi{};
    avb_decoder_get_media_info(dec, &mi);
    check(mi.video.width == 320 && mi.video.height == 240, "memory: media info correct");
    check(mi.audio.available == 1, "memory: audio available");

    // --- audio block timestamps (P2) ---
    printf("audio timestamps:\n");
    double first_pts = -123.0;
    std::vector<float> buf(4096 * (mi.audio.channels > 0 ? mi.audio.channels : 1));
    int got0 = avb_decoder_read_audio_f32(dec, buf.data(), 4096, &first_pts);
    check(got0 > 0, "first audio read returns frames");
    check_near(first_pts, 0.0, 0.05, "first block pts ~0.0");

    double pts1 = -1.0;
    avb_decoder_read_audio_f32(dec, buf.data(), 4096, &pts1);
    check_near(pts1, (double)got0 / mi.audio.sample_rate, 0.05,
               "second block pts advances by first block length");

    // --- seek landed time (P3) ---
    printf("seek landed:\n");
    double landed = -1.0;
    check(avb_decoder_seek(dec, 1.5, &landed) == AVB_OK, "seek(1.5) succeeds");
    check_near(landed, 1.5, 0.001, "landed time == clamped request");

    double seek_pts = -1.0;
    avb_decoder_read_audio_f32(dec, buf.data(), 4096, &seek_pts);
    check_near(seek_pts, 1.5, 0.3, "audio first pts after seek ~1.5s");

    // out-of-range seek clamps to duration
    double landed2 = -1.0;
    avb_decoder_seek(dec, 999.0, &landed2);
    check_near(landed2, mi.duration_sec, 0.01, "seek past end clamps to duration");

    avb_decoder_close(dec);

    // --- audio EOF distinction (P4) ---
    // Use an audio-only decoder: draining one stream to EOF while never pulling
    // the other is unsupported on a dual-stream decoder (bounded video sink).
    printf("audio EOF:\n");
    {
        avb_decode_options ao = avb_decode_options_default();
        ao.backend = backend;
        ao.enable_video = 0;
        avb_decoder *adec = nullptr;
        check(avb_decoder_open_memory(&adec, bytes.data(), bytes.size(), &ao) == AVB_OK,
              "open audio-only from memory");
        check(avb_decoder_audio_at_eof(adec) == 0, "not at EOF before draining");
        for (;;) {
            int g = avb_decoder_read_audio_f32(adec, buf.data(), 4096, nullptr);
            if (g <= 0) break;
        }
        check(avb_decoder_audio_at_eof(adec) == 1, "at EOF after draining audio");
        avb_decoder_seek(adec, 0.0, nullptr);
        check(avb_decoder_audio_at_eof(adec) == 0, "EOF flag reset by seek");
        avb_decoder_close(adec);
    }

    if (backend == AVB_BACKEND_FFMPEG) {
        check_read_errors(bytes, false);
        check_read_errors(bytes, true);
    }

    printf("\n%s (%d failure%s)\n",
           g_failures == 0 ? "IO CHECKS PASSED" : "IO CHECKS FAILED",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
