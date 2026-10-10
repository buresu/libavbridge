#include "test.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace {
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
    check_audio_seek(test, argv[2]);
    if (argc > 3) check_ivf(test, argv[3]);
    return test.finish("Media Foundation decode contracts");
}
