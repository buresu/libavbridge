#include <avbridge.h>
#include <cstdio>
#include <cstring>

// Writes a single BGRA frame as a 32-bit BMP file.
// BMP stores rows bottom-up, so we flip here.
static bool write_bmp(const char *path, const avb_video_frame *frame) {
    int w = frame->width;
    int h = frame->height;
    int row_bytes = w * 4;
    int pixel_data_size = row_bytes * h;

    // BMP file header (14 bytes) + DIB header (40 bytes)
    unsigned char hdr[54] = {};
    int file_size = 54 + pixel_data_size;

    // File header
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = (file_size)       & 0xff;
    hdr[3] = (file_size >> 8)  & 0xff;
    hdr[4] = (file_size >> 16) & 0xff;
    hdr[5] = (file_size >> 24) & 0xff;
    hdr[10] = 54; // pixel data offset

    // DIB header (BITMAPINFOHEADER)
    hdr[14] = 40; // header size
    hdr[18] = w & 0xff; hdr[19] = (w >> 8) & 0xff;
    hdr[20] = (w >> 16) & 0xff; hdr[21] = (w >> 24) & 0xff;
    // Negative height = top-down (matches our buffer order)
    int neg_h = -h;
    hdr[22] = neg_h & 0xff; hdr[23] = (neg_h >> 8) & 0xff;
    hdr[24] = (neg_h >> 16) & 0xff; hdr[25] = (neg_h >> 24) & 0xff;
    hdr[26] = 1;  // color planes
    hdr[28] = 32; // bits per pixel
    // compression = 0 (BI_RGB), but BMP with 32bpp is treated as BGRA natively

    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fwrite(hdr, 1, 54, f);
    fwrite(frame->data, 1, pixel_data_size, f);
    fclose(f);
    return true;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: avb_decode_video <input_file> <output_pattern>\n");
        fprintf(stderr, "  output_pattern: path with %%04d placeholder, e.g. frame_%%04d.bmp\n");
        return 1;
    }

    avb_decode_options opts{};
    opts.backend            = AVB_BACKEND_AUTO;
    opts.audio_stream_index = -1;
    opts.video_stream_index = -1;
    opts.enable_audio       = 0;
    opts.enable_video       = 1;

    avb_decoder *ctx = nullptr;
    avb_result res = avb_decoder_open(&ctx, argv[1], &opts);
    if (res != AVB_OK) {
        fprintf(stderr, "avb_decoder_open failed (%d): %s\n", res,
                avb_decoder_get_last_error(ctx) ? avb_decoder_get_last_error(ctx) : "unknown");
        avb_decoder_close(ctx);
        return 1;
    }

    avb_media_info info{};
    avb_decoder_get_media_info(ctx, &info);

    if (!info.video.available) {
        fprintf(stderr, "No video stream found in: %s\n", argv[1]);
        avb_decoder_close(ctx);
        return 1;
    }

    printf("Decoding video: %s\n", argv[1]);
    printf("  Codec      : %s\n", info.video.codec_name ? info.video.codec_name : "unknown");
    printf("  Resolution : %dx%d\n", info.video.width, info.video.height);
    printf("  Frame rate : %.3f fps\n", info.video.frame_rate);
    printf("  Duration   : %.3f sec\n", info.video.duration_sec);

    char out_path[1024];
    int frame_idx = 0;

    while (true) {
        avb_video_frame frame{};
        avb_result fres = avb_decoder_read_video_frame(ctx, &frame);
        if (fres == AVB_ERROR_EOF) break;
        if (fres != AVB_OK) {
            fprintf(stderr, "read_video_frame failed (%d): %s\n", fres,
                    avb_decoder_get_last_error(ctx) ? avb_decoder_get_last_error(ctx) : "unknown");
            break;
        }

        snprintf(out_path, sizeof(out_path), argv[2], frame_idx);
        if (!write_bmp(out_path, &frame)) {
            fprintf(stderr, "Failed to write: %s\n", out_path);
        } else {
            printf("  Frame %4d  pts=%.3f sec  -> %s\n", frame_idx, frame.pts_sec, out_path);
        }

        avb_decoder_release_video_frame(ctx, &frame);
        frame_idx++;
    }

    printf("Done. Decoded %d frames.\n", frame_idx);
    avb_decoder_close(ctx);
    return 0;
}
