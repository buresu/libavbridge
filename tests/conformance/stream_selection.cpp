#include "test.hpp"

#include <vector>

namespace {
int plugin_stream = -1;
int plugin_width = 0;
int packet_stream = -1;
unsigned char block[8]{};

int can_decode(const avb_video_stream_info *, const avb_decode_options *options) {
    return options->video_format == AVB_PIXEL_FORMAT_BC1_RGBA;
}
avb_result open_plugin(void **ctx, const avb_video_stream_info *stream,
                       const avb_decode_options *) {
    plugin_stream = stream->stream_index;
    plugin_width = stream->width;
    *ctx = block;
    return AVB_OK;
}
avb_result decode_packet(void *, const avb_encoded_packet *packet,
                         avb_video_frame *frame) {
    packet_stream = packet->stream_index;
    *frame = {};
    frame->pts_sec = packet->pts_sec;
    frame->width = plugin_width;
    frame->format = AVB_PIXEL_FORMAT_BC1_RGBA;
    frame->data = block;
    frame->data_size = sizeof(block);
    return AVB_OK;
}
void release_frame(void *, avb_video_frame *) {}
void close_plugin(void *) {}

void check_selection(avb::test::Context &test, const char *path, bool custom,
                     bool audio, int track) {
    std::printf("selection: custom=%d audio=%d track=%d\n", custom, audio, track);
    std::fflush(stdout);
    avb_decode_options options = avb_decode_options_default();
    options.backend = AVB_BACKEND_GSTREAMER;
    options.hardware_policy = AVB_HARDWARE_DISABLED;
    options.enable_audio = audio;
    options.video_stream_index = track;
    options.audio_stream_index = track;
    options.video_format = custom ? AVB_PIXEL_FORMAT_BC1_RGBA : AVB_PIXEL_FORMAT_BGRA8;
    avb_decoder *decoder = nullptr;
    const avb_result opened = avb_decoder_open(&decoder, path, &options);
    test.equal(opened, AVB_OK, "selected tracks open");
    if (opened != AVB_OK) {
        std::fprintf(stderr, "%s\n", avb::test::decoder_error(decoder));
        avb_decoder_close(decoder);
        return;
    }
    avb_media_info info{};
    avb_decoder_get_media_info(decoder, &info);
    test.equal(info.video.stream_index, track, "video reports selected logical index");
    test.equal(info.video.width, track ? 96 : 64, "video metadata is from selected track");
    test.near(info.video.frame_rate, track ? 15 : 10, 0.001, "selected video rate is reported");
    test.string(info.video.codec_name, track ? "mpeg4" : "h264", "selected video codec is reported");
    if (audio) {
        test.equal(info.audio.track_count, 2, "all audio tracks are counted");
        test.equal(info.audio.stream_index, track, "audio reports selected logical index");
        test.equal(info.audio.sample_rate, track ? 48000 : 32000, "selected audio rate is reported");
        test.equal(info.audio.channels, track ? 2 : 1, "selected audio channels are reported");
    }
    for (int pass = 0; pass < 2; ++pass) {
        if (pass) test.equal(avb_decoder_seek(decoder, 0.5, nullptr), AVB_OK, "selected tracks seek");
        avb_video_frame frame{};
        const avb_result read = avb_decoder_read_video_frame(decoder, &frame);
        test.equal(read, AVB_OK, "selected video decodes");
        if (read == AVB_OK) {
            test.equal(frame.width, track ? 96 : 64, "decoded frame is from selected track");
            if (custom) {
                test.equal(plugin_stream, track, "plugin opens selected stream index");
                test.equal(packet_stream, track, "plugin receives selected packet index");
            } else {
                test.check(track ? frame.data[0] > frame.data[2]
                                 : frame.data[2] > frame.data[0],
                           "selected video has the expected blue/red pixels");
            }
            avb_decoder_release_video_frame(decoder, &frame);
        }
        if (audio) {
            std::vector<float> samples(1024 * 2);
            test.equal(avb_decoder_read_audio_f32(decoder, samples.data(), 1024, nullptr),
                       1024, "selected audio decodes");
        }
    }
    avb_decoder_close(decoder);
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    avb::test::Context test;
    avb_video_decoder_plugin plugin{};
    plugin.struct_size = sizeof(plugin);
    plugin.name = "track-selection";
    plugin.can_decode = can_decode;
    plugin.open = open_plugin;
    plugin.decode_packet = decode_packet;
    plugin.release_frame = release_frame;
    plugin.close = close_plugin;
    test.equal(avb_register_video_decoder(&plugin), AVB_OK, "register selection plugin");
    for (bool custom : {false, true}) {
        for (bool audio : {false, true})
            for (int track : {0, 1}) check_selection(test, argv[1], custom, audio, track);
        for (bool audio : {false, true}) {
            avb_decode_options options = avb_decode_options_default();
            options.backend = AVB_BACKEND_GSTREAMER;
            options.hardware_policy = AVB_HARDWARE_DISABLED;
            options.video_format = custom ? AVB_PIXEL_FORMAT_BC1_RGBA : AVB_PIXEL_FORMAT_BGRA8;
            if (audio) options.audio_stream_index = 999;
            else options.video_stream_index = 999;
            avb_decoder *decoder = nullptr;
            test.equal(avb_decoder_open(&decoder, argv[1], &options), AVB_ERROR_STREAM_NOT_FOUND,
                       "out-of-range explicit track is rejected");
            test.check(avb_decoder_get_last_error(decoder) != nullptr, "missing track has a diagnostic");
            avb_decoder_close(decoder);
        }
    }
    avb_unregister_video_decoder(&plugin);
    return test.finish("stream selection");
}
