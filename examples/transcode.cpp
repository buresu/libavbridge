// Transcode example: decode an input file and re-encode it to MP4 (H.264 + AAC)
// using the avbridge encoder. Demonstrates feeding decoded frames straight into
// the encoder, interleaving audio and video roughly in increasing-PTS order.
//
// Usage: avb_transcode <input> <output.mp4>

#include <avbridge.h>
#include <cstdio>
#include <vector>

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <input> <output.mp4>\n", argv[0]);
        return 1;
    }

    // --- Open the source for decode (BGRA video + audio) ---
    avb_decode_options dopts{};
    dopts.backend            = AVB_BACKEND_AUTO;
    dopts.audio_stream_index = -1;
    dopts.video_stream_index = -1;
    dopts.enable_audio       = 1;
    dopts.enable_video       = 1;
    dopts.video_format       = AVB_PIXEL_FORMAT_BGRA8;

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
    eopts.backend = AVB_BACKEND_AUTO;
    if (info.video.available) {
        eopts.video.enable       = 1;
        eopts.video.width        = info.video.width;
        eopts.video.height       = info.video.height;
        eopts.video.frame_rate   = info.video.frame_rate;
        eopts.video.codec        = AVB_CODEC_AUTO;
        eopts.video.bitrate      = 2000000;
        eopts.video.input_format = AVB_PIXEL_FORMAT_BGRA8;
    }
    if (info.audio.available) {
        eopts.audio.enable      = 1;
        eopts.audio.sample_rate = info.audio.sample_rate;
        eopts.audio.channels    = info.audio.channels;
        eopts.audio.codec       = AVB_CODEC_AUTO;
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
            int got = avb_decoder_read_audio_f32(dec, pcm.data(), 1024);
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
        int got = avb_decoder_read_audio_f32(dec, pcm.data(), 1024);
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
