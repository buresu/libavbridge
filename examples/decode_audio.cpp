#include <avbridge.h>
#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: avb_decode_audio <input_file> <output.f32>\n");
        fprintf(stderr, "Output is raw interleaved float32 PCM.\n");
        return 1;
    }

    avb_open_options opts{};
    opts.backend            = AVB_BACKEND_AUTO;
    opts.audio_stream_index = -1;
    opts.video_stream_index = -1;
    opts.enable_audio       = 1;
    opts.enable_video       = 0;

    avb_context *ctx = nullptr;
    avb_result res = avb_open_file(&ctx, argv[1], &opts);
    if (res != AVB_OK) {
        fprintf(stderr, "avb_open_file failed (%d): %s\n", res,
                avb_get_last_error(ctx) ? avb_get_last_error(ctx) : "unknown error");
        avb_close(ctx);
        return 1;
    }

    avb_media_info info{};
    avb_get_media_info(ctx, &info);

    if (!info.audio.available) {
        fprintf(stderr, "No audio stream found in: %s\n", argv[1]);
        avb_close(ctx);
        return 1;
    }

    printf("Decoding audio: %s\n", argv[1]);
    printf("  Codec       : %s\n", info.audio.codec_name ? info.audio.codec_name : "unknown");
    printf("  Sample rate : %d Hz\n", info.audio.sample_rate);
    printf("  Channels    : %d\n", info.audio.channels);
    printf("  Duration    : %.3f sec\n", info.audio.duration_sec);
    printf("Output        : %s\n", argv[2]);

    FILE *out = fopen(argv[2], "wb");
    if (!out) {
        fprintf(stderr, "Cannot open output file: %s\n", argv[2]);
        avb_close(ctx);
        return 1;
    }

    const int BLOCK_FRAMES = 4096;
    std::vector<float> buf(BLOCK_FRAMES * info.audio.channels);
    int total_frames = 0;

    while (true) {
        int got = avb_read_audio_f32(ctx, buf.data(), BLOCK_FRAMES);
        if (got <= 0) break;
        fwrite(buf.data(), sizeof(float), got * info.audio.channels, out);
        total_frames += got;
    }

    fclose(out);
    printf("Done. Decoded %d frames (%.3f sec).\n", total_frames,
           (double)total_frames / info.audio.sample_rate);

    avb_close(ctx);
    return 0;
}
