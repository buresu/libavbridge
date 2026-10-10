#include "test.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace {
struct PrerollDecoder {
    bool has_keyframe = false;
    int decoded = 0;
    int released = 0;
    unsigned char block[8]{};
} preroll;

int can_decode_preroll(const avb_video_stream_info *, const avb_decode_options *options) {
    return options->video_format == AVB_PIXEL_FORMAT_BC1_RGBA;
}
avb_result open_preroll(void **context, const avb_video_stream_info *, const avb_decode_options *) {
    preroll = {};
    *context = &preroll;
    return AVB_OK;
}
avb_result decode_preroll(void *, const avb_encoded_packet *packet, avb_video_frame *frame) {
    if (packet->keyframe) preroll.has_keyframe = true;
    if (!preroll.has_keyframe) return AVB_ERROR_DECODE_FAILED;
    ++preroll.decoded;
    *frame = {};
    frame->width = 4;
    frame->height = 4;
    frame->format = AVB_PIXEL_FORMAT_BC1_RGBA;
    frame->data = preroll.block;
    frame->data_size = sizeof(preroll.block);
    frame->pts_sec = packet->pts_sec;
    return AVB_OK;
}
void release_preroll(void *, avb_video_frame *) { ++preroll.released; }
void flush_preroll(void *) { preroll.has_keyframe = false; }
void close_preroll(void *) {}

void check_custom_seek(avb::test::Context &test, const char *path) {
    avb_video_decoder_plugin plugin{};
    plugin.struct_size = sizeof(plugin);
    plugin.name = "stateful-preroll";
    plugin.can_decode = can_decode_preroll;
    plugin.open = open_preroll;
    plugin.decode_packet = decode_preroll;
    plugin.release_frame = release_preroll;
    plugin.flush = flush_preroll;
    plugin.close = close_preroll;
    test.equal(avb_register_video_decoder(&plugin), AVB_OK, "register stateful decoder");
    auto options = avb_decode_options_default();
    options.backend = AVB_BACKEND_MEDIAFOUNDATION;
    options.enable_audio = 0;
    options.video_format = AVB_PIXEL_FORMAT_BC1_RGBA;
    avb_decoder *decoder = nullptr;
    const auto opened = avb_decoder_open(&decoder, path, &options);
    test.equal(opened, AVB_OK, "stateful custom decoder opens");
    if (opened == AVB_OK) {
        for (double target : {0.501, 1.501}) {
            test.equal(avb_decoder_seek(decoder, target, nullptr), AVB_OK, "custom decoder seeks");
            avb_video_frame frame{};
            const auto read = avb_decoder_read_video_frame(decoder, &frame);
            test.equal(read, AVB_OK, "custom decoder receives the keyframe and preroll after seek");
            if (read == AVB_OK) {
                test.check(frame.pts_sec >= target - 1e-6, "custom seek only returns frames at the target");
                avb_decoder_release_video_frame(decoder, &frame);
            }
            test.equal(preroll.decoded, preroll.released, "discarded custom frames are released");
        }
    }
    avb_decoder_close(decoder);
    avb_unregister_video_decoder(&plugin);
}

void check_selection(avb::test::Context &test, const char *path) {
    auto options = avb_decode_options_default();
    options.backend = AVB_BACKEND_MEDIAFOUNDATION;
    avb_decoder *decoder = nullptr;
    const auto opened = avb_decoder_open(&decoder, path, &options);
    test.equal(opened, AVB_OK, "default streams open");
    if (opened != AVB_OK) {
        avb_decoder_close(decoder);
        return;
    }
    avb_media_info info{};
    avb_decoder_get_media_info(decoder, &info);
    avb_decoder_close(decoder);
    for (bool audio : {false, true}) {
        for (int index : {999, audio ? info.video.stream_index : info.audio.stream_index}) {
            auto selected = options;
            if (audio) selected.audio_stream_index = index;
            else selected.video_stream_index = index;
            test.equal(avb_decoder_open(&decoder, path, &selected), AVB_ERROR_STREAM_NOT_FOUND,
                       "missing or wrong-kind explicit stream is rejected");
            test.check(avb_decoder_get_last_error(decoder) != nullptr, "selection failure has diagnostic");
            avb_decoder_close(decoder);
        }
        auto selected = options;
        selected.enable_audio = audio;
        selected.enable_video = !audio;
        selected.audio_stream_index = info.audio.stream_index;
        selected.video_stream_index = info.video.stream_index;
        test.equal(avb_decoder_open(&decoder, path, &selected), AVB_OK, "disabled stream index is ignored");
        avb_media_info selected_info{};
        avb_decoder_get_media_info(decoder, &selected_info);
        test.equal(selected_info.audio.available, int(audio), "audio enable is respected");
        test.equal(selected_info.video.available, int(!audio), "video enable is respected");
        avb_decoder_close(decoder);
    }
}

void check_unsupported_format(avb::test::Context &test, const char *path) {
    auto options = avb_decode_options_default();
    options.backend = AVB_BACKEND_MEDIAFOUNDATION;
    options.enable_audio = 0;
    options.video_format = AVB_PIXEL_FORMAT_BC1_RGBA;
    options.enable_custom_video_decoders = 0;
    avb_decoder *decoder = nullptr;
    test.check(avb_decoder_open(&decoder, path, &options) != AVB_OK,
               "unsupported compressed output does not silently become BGRA");
    avb_decoder_close(decoder);
}

void check_audio_seek(avb::test::Context &test, const char *path) {
    auto options = avb_decode_options_default();
    options.backend = AVB_BACKEND_MEDIAFOUNDATION;
    options.enable_video = 0;
    avb_decoder *decoder = nullptr;
    const auto opened = avb_decoder_open(&decoder, path, &options);
    test.equal(opened, AVB_OK, "audio opens");
    if (opened != AVB_OK) {
        avb_decoder_close(decoder);
        return;
    }
    avb_media_info info{};
    avb_decoder_get_media_info(decoder, &info);
    std::vector<float> samples(257 * info.audio.channels);
    test.equal(avb_decoder_seek(decoder, 0.501, nullptr), AVB_OK, "audio seeks inside a sample");
    double pts = -1.0;
    test.equal(avb_decoder_read_audio_f32(decoder, samples.data(), 257, &pts), 257, "audio reads after seek");
    test.near(pts, 0.501, 1.0 / info.audio.sample_rate, "seek retains audio after target within its block");
    avb_decoder_close(decoder);
}

void check_ivf(avb::test::Context &test, const char *path) {
    avb_decoder_capabilities caps{};
    avb_decoder_probe_runtime_capabilities(AVB_BACKEND_MEDIAFOUNDATION, path, &caps);
    if (!avb::test::contains(caps.video_codecs, caps.video_codec_count, AVB_VIDEO_CODEC_VP8)) {
        std::printf("SKIP: VP8 decoder MFT is unavailable\n");
        return;
    }
    auto options = avb_decode_options_default();
    options.backend = AVB_BACKEND_MEDIAFOUNDATION;
    options.enable_audio = 0;
    avb_decoder *decoder = nullptr;
    const auto opened = avb_decoder_open(&decoder, path, &options);
    test.equal(opened, AVB_OK, "VP8 IVF opens");
    if (opened != AVB_OK) {
        std::fprintf(stderr, "%s\n", avb::test::decoder_error(decoder));
        avb_decoder_close(decoder);
        return;
    }
    avb_video_frame frame{};
    test.equal(avb_decoder_seek(decoder, 39.9, nullptr), AVB_OK, "long IVF seek succeeds");
    const auto read = avb_decoder_read_video_frame(decoder, &frame);
    test.equal(read, AVB_OK, "long seek decodes without recursive preroll");
    if (read == AVB_OK) {
        test.check(frame.pts_sec >= 39.9 - 1e-6, "IVF seek reaches target");
        avb_decoder_release_video_frame(decoder, &frame);
    }
    while (avb_decoder_read_video_frame(decoder, &frame) == AVB_OK)
        avb_decoder_release_video_frame(decoder, &frame);
    test.equal(avb_decoder_read_video_frame(decoder, &frame), AVB_ERROR_EOF, "IVF EOF remains stable");
    test.equal(avb_decoder_seek(decoder, 0.0, nullptr), AVB_OK, "IVF restarts after EOF");
    test.equal(avb_decoder_read_video_frame(decoder, &frame), AVB_OK, "IVF reads after restart");
    avb_decoder_release_video_frame(decoder, &frame);
    avb_decoder_close(decoder);

    const auto unicode_path = std::filesystem::u8path(path).parent_path() / L"\u52d5\u753b-\u03b1.ivf";
    std::filesystem::copy_file(path, unicode_path, std::filesystem::copy_options::overwrite_existing);
    test.equal(avb_decoder_open(&decoder, unicode_path.u8string().c_str(), &options), AVB_OK,
               "UTF-8 IVF path opens");
    avb_decoder_close(decoder);
    std::filesystem::remove(unicode_path);

    std::ifstream input(path, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), {});
    test.equal(avb_decoder_open_memory(&decoder, bytes.data(), bytes.size(), &options), AVB_OK,
               "IVF memory input is recognized");
    avb_decoder_close(decoder);
    bytes.push_back(1); // A partial packet header is corruption, not clean EOF.
    test.equal(avb_decoder_open_memory(&decoder, bytes.data(), bytes.size(), &options), AVB_OK,
               "IVF with truncated final header opens");
    avb_result result = AVB_OK;
    for (int i = 0; i < 1300 && result == AVB_OK; ++i) {
        result = avb_decoder_read_video_frame(decoder, &frame);
        if (result == AVB_OK) avb_decoder_release_video_frame(decoder, &frame);
    }
    test.equal(result, AVB_ERROR_DECODE_FAILED, "truncated IVF header reports a read error");
    test.check(avb_decoder_get_last_error(decoder) != nullptr, "truncated IVF has a diagnostic");
    avb_decoder_close(decoder);
    options.video_stream_index = 1;
    test.equal(avb_decoder_open(&decoder, path, &options), AVB_ERROR_STREAM_NOT_FOUND,
               "IVF rejects nonexistent video stream");
    avb_decoder_close(decoder);
}
} // namespace

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    avb::test::Context test;
    check_selection(test, argv[1]);
    check_custom_seek(test, argv[1]);
    check_audio_seek(test, argv[2]);
    check_unsupported_format(test, argv[1]);
    if (argc > 3) {
        check_ivf(test, argv[3]);
        check_unsupported_format(test, argv[3]);
    }
    return test.finish("Media Foundation decode contracts");
}
