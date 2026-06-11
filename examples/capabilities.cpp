#include <avbridge.h>

#include <cstdio>
#include <cstring>

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [--backend name] [--path media-or-output-path]\n"
        "          [--static] [--runtime] [--decoder] [--encoder]\n\n"
        "Defaults: backend=auto, no path, static+runtime, decoder+encoder.\n",
        argv0);
}

static const char *pixel_format_name(avb_pixel_format format) {
    switch (format) {
        case AVB_PIXEL_FORMAT_UNKNOWN:  return "unknown";
        case AVB_PIXEL_FORMAT_RGBA8:    return "rgba8";
        case AVB_PIXEL_FORMAT_BGRA8:    return "bgra8";
        case AVB_PIXEL_FORMAT_NV12:     return "nv12";
        case AVB_PIXEL_FORMAT_I420:     return "i420";
        case AVB_PIXEL_FORMAT_BC1_RGBA: return "bc1_rgba";
        case AVB_PIXEL_FORMAT_BC3_RGBA: return "bc3_rgba";
        case AVB_PIXEL_FORMAT_BC4_R:    return "bc4_r";
        case AVB_PIXEL_FORMAT_BC5_RG:   return "bc5_rg";
        case AVB_PIXEL_FORMAT_BC7_RGBA: return "bc7_rgba";
    }
    return "invalid";
}

static const char *video_memory_name(avb_video_memory_type memory) {
    switch (memory) {
        case AVB_VIDEO_MEMORY_CPU:    return "cpu";
        case AVB_VIDEO_MEMORY_NATIVE: return "native";
        case AVB_VIDEO_MEMORY_DMABUF: return "dmabuf";
    }
    return "invalid";
}

static const char *hardware_device_name(avb_hardware_device device) {
    switch (device) {
        case AVB_HW_DEVICE_AUTO:         return "auto";
        case AVB_HW_DEVICE_VAAPI:        return "vaapi";
        case AVB_HW_DEVICE_CUDA:         return "cuda";
        case AVB_HW_DEVICE_QSV:          return "qsv";
        case AVB_HW_DEVICE_D3D11VA:      return "d3d11va";
        case AVB_HW_DEVICE_VIDEOTOOLBOX: return "videotoolbox";
        case AVB_HW_DEVICE_AMF:          return "amf";
    }
    return "invalid";
}

template <typename T, typename NameFn>
static void print_list(const char *label, const T *values, int count, NameFn name_fn) {
    printf("  %-16s: ", label);
    if (count <= 0) {
        printf("(none)\n");
        return;
    }
    for (int i = 0; i < count; ++i) {
        if (i) printf(", ");
        const char *name = name_fn(values[i]);
        printf("%s", name ? name : "invalid");
    }
    printf("\n");
}

static void print_decoder_caps(const char *title,
                               const avb_decoder_capabilities &caps) {
    printf("\n%s decoder:\n", title);
    printf("  result          : %s\n", avb_result_string(caps.result));
    printf("  backend         : %s\n", caps.backend_name ? caps.backend_name : "(invalid)");
    printf("  container       : %s\n", caps.container_name ? caps.container_name : "(unknown)");
    printf("  message         : %s\n", caps.message ? caps.message : "(none)");
    printf("  video           : %s\n", caps.can_decode_video ? "yes" : "no");
    printf("  audio           : %s\n", caps.can_decode_audio ? "yes" : "no");
    print_list("video codecs", caps.video_codecs, caps.video_codec_count,
               avb_video_codec_name);
    print_list("audio codecs", caps.audio_codecs, caps.audio_codec_count,
               avb_audio_codec_name);
    print_list("pixel formats", caps.pixel_formats, caps.pixel_format_count,
               pixel_format_name);
    print_list("video memory", caps.video_memory, caps.video_memory_count,
               video_memory_name);
    print_list("hw devices", caps.hardware_devices, caps.hardware_device_count,
               hardware_device_name);
}

static void print_encoder_caps(const char *title,
                               const avb_encoder_capabilities &caps) {
    printf("\n%s encoder:\n", title);
    printf("  result          : %s\n", avb_result_string(caps.result));
    printf("  backend         : %s\n", caps.backend_name ? caps.backend_name : "(invalid)");
    printf("  container       : %s\n", caps.container_name ? caps.container_name : "(unknown)");
    printf("  message         : %s\n", caps.message ? caps.message : "(none)");
    printf("  video           : %s\n", caps.can_encode_video ? "yes" : "no");
    printf("  audio           : %s\n", caps.can_encode_audio ? "yes" : "no");
    print_list("video codecs", caps.video_codecs, caps.video_codec_count,
               avb_video_codec_name);
    print_list("audio codecs", caps.audio_codecs, caps.audio_codec_count,
               avb_audio_codec_name);
    print_list("video memory", caps.video_memory, caps.video_memory_count,
               video_memory_name);
    print_list("hw devices", caps.hardware_devices, caps.hardware_device_count,
               hardware_device_name);
}

int main(int argc, char *argv[]) {
    avb_backend backend = AVB_BACKEND_AUTO;
    const char *path = nullptr;
    bool show_static = false;
    bool show_runtime = false;
    bool show_decoder = false;
    bool show_encoder = false;

    for (int i = 1; i < argc; ++i) {
        auto need_value = [&](const char *opt) -> const char * {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires a value\n", opt);
                return nullptr;
            }
            return argv[++i];
        };

        if (std::strcmp(argv[i], "--help") == 0 ||
            std::strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (std::strcmp(argv[i], "--backend") == 0) {
            const char *value = need_value(argv[i]);
            if (!value || avb_backend_from_name(value, &backend) != AVB_OK) {
                fprintf(stderr, "unknown backend '%s'\n", value ? value : "");
                return 2;
            }
        } else if (std::strcmp(argv[i], "--path") == 0) {
            path = need_value(argv[i]);
            if (!path) return 2;
        } else if (std::strcmp(argv[i], "--static") == 0) {
            show_static = true;
        } else if (std::strcmp(argv[i], "--runtime") == 0) {
            show_runtime = true;
        } else if (std::strcmp(argv[i], "--decoder") == 0) {
            show_decoder = true;
        } else if (std::strcmp(argv[i], "--encoder") == 0) {
            show_encoder = true;
        } else {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    if (!show_static && !show_runtime) {
        show_static = true;
        show_runtime = true;
    }
    if (!show_decoder && !show_encoder) {
        show_decoder = true;
        show_encoder = true;
    }

    printf("libavbridge %s\n", avb_version_string());
    printf("request backend : %s\n", avb_backend_name(backend));
    printf("path            : %s\n", path ? path : "(none)");

    if (show_static && show_decoder) {
        avb_decoder_capabilities caps{};
        avb_result res = avb_decoder_query_capabilities(backend, path, &caps);
        if (res != AVB_OK) {
            fprintf(stderr, "avb_decoder_query_capabilities failed: %s\n",
                    avb_result_string(res));
            return 1;
        }
        print_decoder_caps("Static", caps);
    }

    if (show_runtime && show_decoder) {
        avb_decoder_capabilities caps{};
        avb_result res = avb_decoder_probe_runtime_capabilities(backend, path, &caps);
        if (res != AVB_OK) {
            fprintf(stderr, "avb_decoder_probe_runtime_capabilities failed: %s\n",
                    avb_result_string(res));
            return 1;
        }
        print_decoder_caps("Runtime", caps);
    }

    if (show_static && show_encoder) {
        avb_encoder_capabilities caps{};
        avb_result res = avb_encoder_query_capabilities(backend, path, &caps);
        if (res != AVB_OK) {
            fprintf(stderr, "avb_encoder_query_capabilities failed: %s\n",
                    avb_result_string(res));
            return 1;
        }
        print_encoder_caps("Static", caps);
    }

    if (show_runtime && show_encoder) {
        avb_encoder_capabilities caps{};
        avb_result res = avb_encoder_probe_runtime_capabilities(backend, path, &caps);
        if (res != AVB_OK) {
            fprintf(stderr, "avb_encoder_probe_runtime_capabilities failed: %s\n",
                    avb_result_string(res));
            return 1;
        }
        print_encoder_caps("Runtime", caps);
    }

    return 0;
}
