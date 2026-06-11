// Transcode example: decode an input file and re-encode it using the avbridge
// encoder. Demonstrates feeding decoded frames straight into the encoder,
// interleaving audio and video roughly in increasing-PTS order.
//
// Usage:
//   avb_transcode <input> <output>
//       [--backend auto|gstreamer|ffmpeg|mediafoundation|avfoundation]
//       [--video-codec auto|h264|hevc|vp8|vp9|av1|hap]
//       [--audio-codec auto|aac|opus]
//       [--video-bitrate bits-per-sec] [--audio-bitrate bits-per-sec]
//       [--hardware disabled|prefer|require]
//       [--hardware-device auto|vaapi|cuda|qsv|d3d11va|videotoolbox|amf]

#include <avbridge.h>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <vector>

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s <input> <output> "
        "[--backend name] [--video-codec name] [--audio-codec name] "
        "[--video-bitrate bps] [--audio-bitrate bps] "
        "[--hardware disabled|prefer|require] [--hardware-device name]\n",
        argv0);
}

static bool ends_with_ci(const char *path, const char *suffix) {
    size_t plen = std::strlen(path);
    size_t slen = std::strlen(suffix);
    if (plen < slen) return false;
    path += plen - slen;
    for (size_t i = 0; i < slen; ++i) {
        if (std::tolower((unsigned char)path[i]) !=
            std::tolower((unsigned char)suffix[i])) return false;
    }
    return true;
}

static bool parse_codec(const char *name, avb_codec *out) {
    if (std::strcmp(name, "auto") == 0) { *out = AVB_CODEC_AUTO; return true; }
    if (std::strcmp(name, "h264") == 0) { *out = AVB_CODEC_H264; return true; }
    if (std::strcmp(name, "hevc") == 0) { *out = AVB_CODEC_HEVC; return true; }
    if (std::strcmp(name, "vp8")  == 0) { *out = AVB_CODEC_VP8;  return true; }
    if (std::strcmp(name, "vp9")  == 0) { *out = AVB_CODEC_VP9;  return true; }
    if (std::strcmp(name, "av1")  == 0) { *out = AVB_CODEC_AV1;  return true; }
    if (std::strcmp(name, "hap")  == 0) { *out = AVB_CODEC_HAP;  return true; }
    if (std::strcmp(name, "aac")  == 0) { *out = AVB_CODEC_AAC;  return true; }
    if (std::strcmp(name, "opus") == 0) { *out = AVB_CODEC_OPUS; return true; }
    return false;
}

static bool parse_hardware_policy(const char *name, avb_hardware_policy *out) {
    if (std::strcmp(name, "disabled") == 0) { *out = AVB_HARDWARE_DISABLED; return true; }
    if (std::strcmp(name, "prefer")   == 0) { *out = AVB_HARDWARE_PREFER;   return true; }
    if (std::strcmp(name, "require")  == 0) { *out = AVB_HARDWARE_REQUIRE;  return true; }
    return false;
}

static bool parse_hardware_device(const char *name, avb_hardware_device *out) {
    if (std::strcmp(name, "auto") == 0) { *out = AVB_HW_DEVICE_AUTO; return true; }
    if (std::strcmp(name, "vaapi") == 0) { *out = AVB_HW_DEVICE_VAAPI; return true; }
    if (std::strcmp(name, "cuda") == 0) { *out = AVB_HW_DEVICE_CUDA; return true; }
    if (std::strcmp(name, "qsv") == 0) { *out = AVB_HW_DEVICE_QSV; return true; }
    if (std::strcmp(name, "d3d11va") == 0) { *out = AVB_HW_DEVICE_D3D11VA; return true; }
    if (std::strcmp(name, "videotoolbox") == 0) { *out = AVB_HW_DEVICE_VIDEOTOOLBOX; return true; }
    if (std::strcmp(name, "amf") == 0) { *out = AVB_HW_DEVICE_AMF; return true; }
    return false;
}

static bool parse_positive_int(const char *text, int *out) {
    char *end = nullptr;
    long v = std::strtol(text, &end, 10);
    if (!end || *end != '\0' || v <= 0 || v > 2147483647L) return false;
    *out = (int)v;
    return true;
}

static bool is_web_video_codec(avb_codec codec) {
    return codec == AVB_CODEC_VP8 ||
           codec == AVB_CODEC_VP9 ||
           codec == AVB_CODEC_AV1;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    avb_backend backend = AVB_BACKEND_AUTO;
    avb_codec video_codec = AVB_CODEC_AUTO;
    avb_codec audio_codec = AVB_CODEC_AUTO;
    int video_bitrate = 2000000;
    int audio_bitrate = 0;
    avb_hardware_policy hardware_policy = AVB_HARDWARE_DISABLED;
    avb_hardware_device hardware_device = AVB_HW_DEVICE_AUTO;

    for (int i = 3; i < argc; ++i) {
        auto need_value = [&](const char *opt) -> const char * {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires a value\n", opt);
                return nullptr;
            }
            return argv[++i];
        };

        if (std::strcmp(argv[i], "--backend") == 0) {
            const char *value = need_value(argv[i]);
            if (!value || avb_backend_from_name(value, &backend) != AVB_OK) {
                fprintf(stderr, "unknown backend '%s'\n", value ? value : "");
                return 2;
            }
        } else if (std::strcmp(argv[i], "--video-codec") == 0) {
            const char *value = need_value(argv[i]);
            if (!value || !parse_codec(value, &video_codec)) {
                fprintf(stderr, "unknown video codec '%s'\n", value ? value : "");
                return 2;
            }
        } else if (std::strcmp(argv[i], "--audio-codec") == 0) {
            const char *value = need_value(argv[i]);
            if (!value || !parse_codec(value, &audio_codec)) {
                fprintf(stderr, "unknown audio codec '%s'\n", value ? value : "");
                return 2;
            }
        } else if (std::strcmp(argv[i], "--video-bitrate") == 0) {
            const char *value = need_value(argv[i]);
            if (!value || !parse_positive_int(value, &video_bitrate)) {
                fprintf(stderr, "invalid video bitrate '%s'\n", value ? value : "");
                return 2;
            }
        } else if (std::strcmp(argv[i], "--audio-bitrate") == 0) {
            const char *value = need_value(argv[i]);
            if (!value || !parse_positive_int(value, &audio_bitrate)) {
                fprintf(stderr, "invalid audio bitrate '%s'\n", value ? value : "");
                return 2;
            }
        } else if (std::strcmp(argv[i], "--hardware") == 0) {
            const char *value = need_value(argv[i]);
            if (!value || !parse_hardware_policy(value, &hardware_policy)) {
                fprintf(stderr, "unknown hardware policy '%s'\n", value ? value : "");
                return 2;
            }
        } else if (std::strcmp(argv[i], "--hardware-device") == 0) {
            const char *value = need_value(argv[i]);
            if (!value || !parse_hardware_device(value, &hardware_device)) {
                fprintf(stderr, "unknown hardware device '%s'\n", value ? value : "");
                return 2;
            }
        } else {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    if (audio_codec == AVB_CODEC_AUTO && ends_with_ci(argv[2], ".webm"))
        audio_codec = AVB_CODEC_OPUS;
    bool wants_opus = audio_codec == AVB_CODEC_OPUS;

    // --- Open the source for decode ---
    avb_decode_options dopts{};
    dopts.backend            = backend;
    dopts.audio_stream_index = -1;
    dopts.video_stream_index = -1;
    dopts.enable_audio       = 1;
    dopts.enable_video       = 1;
    dopts.video_format       = is_web_video_codec(video_codec)
        ? AVB_PIXEL_FORMAT_I420
        : AVB_PIXEL_FORMAT_BGRA8;
    if (wants_opus) dopts.audio_sample_rate = 48000;

    avb_decoder *dec = nullptr;
    if (avb_decoder_open(&dec, argv[1], &dopts) != AVB_OK) {
        fprintf(stderr, "open input failed: %s\n",
                avb_decoder_get_last_error(dec) ? avb_decoder_get_last_error(dec) : "unknown");
        avb_decoder_close(dec);
        return 1;
    }
    avb_media_info info{};
    avb_decoder_get_media_info(dec, &info);

    // --- Configure the encoder to match the source ---
    avb_encode_options eopts{};
    eopts.backend = backend;
    if (info.video.available) {
        eopts.video.enable       = 1;
        eopts.video.width        = info.video.width;
        eopts.video.height       = info.video.height;
        eopts.video.frame_rate   = info.video.frame_rate;
        eopts.video.codec        = video_codec;
        eopts.video.bitrate      = video_bitrate;
        eopts.video.input_format = dopts.video_format;
        eopts.video.hardware_policy = hardware_policy;
        eopts.video.hardware_device = hardware_device;
    }
    if (info.audio.available) {
        eopts.audio.enable      = 1;
        eopts.audio.sample_rate = info.audio.sample_rate;
        eopts.audio.channels    = info.audio.channels;
        eopts.audio.codec       = audio_codec;
        eopts.audio.bitrate     = audio_bitrate;
    }

    avb_encoder *enc = nullptr;
    if (avb_encoder_open(&enc, argv[2], &eopts) != AVB_OK) {
        fprintf(stderr, "open encoder failed: %s\n",
                avb_encoder_get_last_error(enc) ? avb_encoder_get_last_error(enc) : "unknown");
        avb_encoder_close(enc);
        avb_decoder_close(dec);
        return 1;
    }

    // --- Transcode loop: keep audio roughly aligned with video by PTS ---
    int video_count = 0, audio_count = 0;
    double audio_pts = 0.0;
    std::vector<float> pcm(1024 * (info.audio.available ? info.audio.channels : 1));

    bool have_video = info.video.available;
    avb_video_frame f{};
    if (have_video) have_video = (avb_decoder_read_video_frame(dec, &f) == AVB_OK);

    while (have_video) {
        if (avb_encoder_write_video(enc, &f, f.pts_sec) != AVB_OK) {
            fprintf(stderr, "write video failed: %s\n", avb_encoder_get_last_error(enc));
            avb_decoder_release_video_frame(dec, &f);
            avb_encoder_close(enc); avb_decoder_close(dec); return 1;
        }
        avb_decoder_release_video_frame(dec, &f);
        video_count++;

        have_video = (avb_decoder_read_video_frame(dec, &f) == AVB_OK);
        double target = have_video ? f.pts_sec : 1e9;

        // Feed audio up to the next video frame's timestamp.
        while (info.audio.available && audio_pts <= target) {
            int got = avb_decoder_read_audio_f32(dec, pcm.data(), 1024, nullptr);
            if (got <= 0) break;
            if (avb_encoder_write_audio_f32(enc, pcm.data(), got) != AVB_OK) {
                fprintf(stderr, "write audio failed: %s\n", avb_encoder_get_last_error(enc));
                avb_encoder_close(enc); avb_decoder_close(dec); return 1;
            }
            audio_count += got;
            audio_pts += (double)got / info.audio.sample_rate;
        }
    }

    // Drain any remaining audio (e.g. audio-only input, or tail past last frame).
    while (info.audio.available) {
        int got = avb_decoder_read_audio_f32(dec, pcm.data(), 1024, nullptr);
        if (got <= 0) break;
        if (avb_encoder_write_audio_f32(enc, pcm.data(), got) != AVB_OK) {
            fprintf(stderr, "write audio failed: %s\n", avb_encoder_get_last_error(enc));
            avb_encoder_close(enc); avb_decoder_close(dec); return 1;
        }
        audio_count += got;
    }

    if (avb_encoder_finish(enc) != AVB_OK) {
        fprintf(stderr, "finish failed: %s\n", avb_encoder_get_last_error(enc));
        avb_encoder_close(enc); avb_decoder_close(dec); return 1;
    }

    printf("Transcoded %d video frames and %d audio frames -> %s\n",
           video_count, audio_count, argv[2]);

    avb_encoder_close(enc);
    avb_decoder_close(dec);
    return 0;
}
