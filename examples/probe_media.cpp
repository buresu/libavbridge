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

    avb_decode_options opts{};
    opts.backend            = backend;
    opts.audio_stream_index = -1;
    opts.video_stream_index = -1;
    opts.enable_audio       = 1;
    opts.enable_video       = 1;

    avb_decoder *ctx = nullptr;
    avb_result res = avb_decoder_open(&ctx, argv[1], &opts);
    if (res != AVB_OK) {
        fprintf(stderr, "avb_decoder_open failed (%d): %s\n", res,
                avb_decoder_get_last_error(ctx) ? avb_decoder_get_last_error(ctx) : "unknown error");
        avb_decoder_close(ctx);
        return 1;
    }

    avb_media_info info{};
    avb_decoder_get_media_info(ctx, &info);

    printf("Backend  : %s\n", info.backend_name ? info.backend_name : "unknown");
    printf("Duration : %.3f sec\n", info.duration_sec);

    if (info.audio.available) {
        printf("\nAudio:\n");
        printf("  Stream index : %d\n", info.audio.stream_index);
        printf("  Codec        : %s\n", info.audio.codec_name ? info.audio.codec_name : "unknown");
        printf("  Sample rate  : %d Hz\n", info.audio.sample_rate);
        printf("  Channels     : %d\n", info.audio.channels);
        printf("  Duration     : %.3f sec\n", info.audio.duration_sec);
    } else {
        printf("\nAudio: not available\n");
    }

    if (info.video.available) {
        printf("\nVideo:\n");
        printf("  Stream index : %d\n", info.video.stream_index);
        printf("  Codec        : %s\n", info.video.codec_name ? info.video.codec_name : "unknown");
        printf("  Resolution   : %dx%d\n", info.video.width, info.video.height);
        printf("  Frame rate   : %.3f fps\n", info.video.frame_rate);
        printf("  Duration     : %.3f sec\n", info.video.duration_sec);
    } else {
        printf("\nVideo: not available\n");
    }

    avb_decoder_close(ctx);
    return 0;
}
