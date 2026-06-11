#include <avbridge.h>
#include <cstdio>
#include <cstring>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: avb_probe <media_file> [backend]\n"
                        "       avb_probe <media_file> --backend <backend>\n");
        return 1;
    }

    avb_backend backend = AVB_BACKEND_AUTO;
    if (argc == 3) {
        if (std::strcmp(argv[2], "--backend") == 0) {
            fprintf(stderr, "--backend requires a value\n");
            return 2;
        }
        if (avb_backend_from_name(argv[2], &backend) != AVB_OK) {
            fprintf(stderr, "unknown backend '%s'\n", argv[2]);
            return 2;
        }
    } else if (argc == 4 && std::strcmp(argv[2], "--backend") == 0) {
        if (avb_backend_from_name(argv[3], &backend) != AVB_OK) {
            fprintf(stderr, "unknown backend '%s'\n", argv[3]);
            return 2;
        }
    } else if (argc > 2) {
        fprintf(stderr, "Usage: avb_probe <media_file> [backend]\n"
                        "       avb_probe <media_file> --backend <backend>\n");
        return 2;
    }

    avb_decode_options opts = avb_decode_options_default();
    opts.backend = backend;

    avb_media_probe info{};
    avb_result res = avb_probe_media(argv[1], &opts, &info);
    if (res != AVB_OK) {
        fprintf(stderr, "avb_probe_media failed (%d): %s\n", res,
                info.error[0] ? info.error : "unknown error");
        return 1;
    }

    printf("Backend  : %s\n", info.backend_name[0] ? info.backend_name : "unknown");
    printf("Duration : %.3f sec\n", info.duration_sec);

    if (info.audio.available) {
        printf("\nAudio:\n");
        printf("  Stream index : %d\n", info.audio.stream_index);
        printf("  Codec        : %s\n", info.audio.codec_name[0] ? info.audio.codec_name : "unknown");
        printf("  Sample rate  : %d Hz\n", info.audio.sample_rate);
        printf("  Channels     : %d\n", info.audio.channels);
        printf("  Duration     : %.3f sec\n", info.audio.duration_sec);
    } else {
        printf("\nAudio: not available\n");
    }

    if (info.video.available) {
        printf("\nVideo:\n");
        printf("  Stream index : %d\n", info.video.stream_index);
        printf("  Codec        : %s\n", info.video.codec_name[0] ? info.video.codec_name : "unknown");
        printf("  Resolution   : %dx%d\n", info.video.width, info.video.height);
        printf("  Frame rate   : %.3f fps\n", info.video.frame_rate);
        printf("  Duration     : %.3f sec\n", info.video.duration_sec);
    } else {
        printf("\nVideo: not available\n");
    }

    return 0;
}
