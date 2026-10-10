#include "test.hpp"

#include <filesystem>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace {
unsigned char payload[8]{};
int can_decode(const avb_video_stream_info *, const avb_decode_options *options) {
    return options->video_format == AVB_PIXEL_FORMAT_BC1_RGBA;
}
avb_result open_plugin(void **ctx, const avb_video_stream_info *, const avb_decode_options *) {
    *ctx = payload;
    return AVB_OK;
}
avb_result decode_packet(void *, const avb_encoded_packet *packet, avb_video_frame *frame) {
    *frame = {};
    frame->data = payload;
    frame->data_size = sizeof(payload);
    frame->format = AVB_PIXEL_FORMAT_BC1_RGBA;
    frame->pts_sec = packet->pts_sec;
    return AVB_OK;
}
void release_frame(void *, avb_video_frame *) {}
void close_plugin(void *) {}

void check_failure(avb::test::Context &test, const char *input, const char *output,
                    bool audio, bool custom) {
    std::filesystem::copy_file(input, output, std::filesystem::copy_options::overwrite_existing);
    avb_decode_options options = avb_decode_options_default();
    options.backend = AVB_BACKEND_FFMPEG;
    options.enable_audio = audio;
    options.enable_video = !audio;
    options.hardware_policy = AVB_HARDWARE_DISABLED;
    options.video_format = custom ? AVB_PIXEL_FORMAT_BC1_RGBA : AVB_PIXEL_FORMAT_BGRA8;
    avb_decoder *decoder = nullptr;
    avb_result result = avb_decoder_open(&decoder, output, &options);
    test.equal(result, AVB_OK, "read-ahead input opens");
    if (result == AVB_OK) {
        // This Linux fault test replaces only the descriptor for its own
        // scratch file with a directory descriptor. The next read fails with
        // EISDIR rather than a clean short-file EOF. The fixture exceeds the
        // bounded read-ahead queue, so buffered packets cannot hide the fault.
        int input_fd = -1;
        const auto expected = std::filesystem::canonical(output);
        for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
            std::error_code error;
            if (std::filesystem::read_symlink(entry.path(), error) == expected)
                input_fd = std::stoi(entry.path().filename().string());
        }
        int directory_fd = ::open("/tmp", O_RDONLY | O_DIRECTORY);
        bool injected = input_fd >= 0 && directory_fd >= 0 && dup2(directory_fd, input_fd) == input_fd;
        if (directory_fd >= 0) ::close(directory_fd);
        test.check(injected, "a real file descriptor read error is injected");
        std::vector<float> samples(4096 * 2);
        for (int i = 0; i < 2000; ++i) {
            if (audio) {
                if (avb_decoder_read_audio_f32(decoder, samples.data(), 4096, nullptr) == 0) break;
            } else {
                avb_video_frame frame{};
                result = avb_decoder_read_video_frame(decoder, &frame);
                if (result != AVB_OK) break;
                avb_decoder_release_video_frame(decoder, &frame);
            }
        }
        if (audio) {
            test.equal(avb_decoder_audio_at_eof(decoder), 0, "threaded audio failure is not EOF");
        } else {
            test.equal(result, AVB_ERROR_DECODE_FAILED, "threaded video failure is not EOF");
            avb_video_frame frame{};
            test.equal(avb_decoder_read_video_frame(decoder, &frame), AVB_ERROR_DECODE_FAILED,
                       "threaded failure persists after its queue is empty");
        }
        const char *error = avb_decoder_get_last_error(decoder);
        test.check(error && *error, "reader thread failure reaches the caller's diagnostic");
    }
    avb_decoder_close(decoder);
    std::filesystem::remove(output);
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    avb_video_decoder_plugin plugin{};
    plugin.struct_size = sizeof(plugin);
    plugin.name = "read-error-packet-decoder";
    plugin.can_decode = can_decode;
    plugin.open = open_plugin;
    plugin.decode_packet = decode_packet;
    plugin.release_frame = release_frame;
    plugin.close = close_plugin;
    avb_register_video_decoder(&plugin);
    avb::test::Context test;
    check_failure(test, argv[1], argv[2], false, false);
    check_failure(test, argv[1], argv[2], false, true);
    check_failure(test, argv[1], argv[2], true, false);
    avb_unregister_video_decoder(&plugin);
    return test.finish("FFmpeg read-ahead failures");
}
