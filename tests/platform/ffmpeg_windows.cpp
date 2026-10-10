#include "test.hpp"

#include <filesystem>
#include <vector>

using namespace avb::test;
namespace fs = std::filesystem;

namespace {

bool decode(Context &test, const std::string &path, avb_hardware_device device,
            avb_video_memory_type memory, bool optional) {
    auto options = avb_decode_options_default();
    options.backend = AVB_BACKEND_FFMPEG;
    options.enable_audio = 0;
    options.hardware_policy = optional ? AVB_HARDWARE_REQUIRE : AVB_HARDWARE_DISABLED;
    options.hardware_device = device;
    options.video_memory = memory;
    avb_decoder *decoder = nullptr;
    const auto result = avb_decoder_open(&decoder, path.c_str(), &options);
    if (result != AVB_OK) {
        std::printf("decode device %d: %s\n", device, decoder_error(decoder));
        test.check(optional, "software decoder opens UTF-8 output path");
        avb_decoder_close(decoder);
        return false;
    }
    bool hardware = false;
    for (int i = 0; i < 3; ++i) {
        avb_video_frame frame{};
        const auto read = avb_decoder_read_video_frame(decoder, &frame);
        test.equal(read, AVB_OK, "read decoded video");
        if (read == AVB_OK) {
            test.equal(frame.memory_type, memory, "requested frame memory");
            if (optional) {
                test.equal(frame.hardware_device, device, "requested decode device");
                hardware = frame.hardware_device == device;
            }
            if (memory == AVB_VIDEO_MEMORY_BACKEND_NATIVE)
                test.check(frame.native_handle != nullptr, "native frame has a handle");
            else
                test.check(frame.data != nullptr, "CPU frame contains pixels");
        }
        avb_decoder_release_video_frame(decoder, &frame);
    }
    test.equal(avb_decoder_seek(decoder, 0.04, nullptr), AVB_OK, "seek decoded video");
    avb_video_frame frame{};
    test.equal(avb_decoder_read_video_frame(decoder, &frame), AVB_OK, "read after seek");
    avb_decoder_release_video_frame(decoder, &frame);
    avb_decoder_close(decoder);
    return hardware;
}

bool encode(Context &test, const fs::path &path, avb_hardware_device device,
            avb_hardware_policy policy, bool required) {
    auto options = avb_encode_options_default();
    options.backend = AVB_BACKEND_FFMPEG;
    options.audio.enable = 0;
    options.video.enable = 1;
    options.video.width = 320;
    options.video.height = 240;
    options.video.frame_rate = 30;
    options.video.codec = AVB_VIDEO_CODEC_H264;
    options.video.input_format = AVB_PIXEL_FORMAT_BGRA8;
    options.video.hardware_device = device;
    options.video.hardware_policy = policy;
    avb_encoder *encoder = nullptr;
    const auto result = avb_encoder_open(&encoder, path.u8string().c_str(), &options);
    if (result != AVB_OK) {
        std::printf("encode device %d policy %d: %s\n", device, policy,
                    encoder_error(encoder));
        test.check(!required, "requested encoder opens");
        avb_encoder_close(encoder);
        return false;
    }
    std::vector<unsigned char> pixels(320 * 240 * 4, 128);
    avb_video_frame frame{};
    frame.width = 320;
    frame.height = 240;
    frame.format = AVB_PIXEL_FORMAT_BGRA8;
    frame.memory_type = AVB_VIDEO_MEMORY_CPU;
    frame.data = pixels.data();
    frame.stride = 320 * 4;
    frame.data_size = static_cast<int>(pixels.size());
    for (int i = 0; i < 12; ++i)
        test.equal(avb_encoder_write_video(encoder, &frame, i / 30.0),
                   AVB_OK, "encode CPU frame");
    test.equal(avb_encoder_finish(encoder), AVB_OK, "finish hardware/fallback output");
    avb_encoder_close(encoder);
    test.check(fs::exists(path), "UTF-8 output exists at the intended path");
    decode(test, path.u8string(), AVB_HW_DEVICE_AUTO, AVB_VIDEO_MEMORY_CPU, false);
    return true;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    Context test;
    const fs::path directory = fs::u8path(argv[2]) / fs::path(L"ffmpeg_\u65e5\u672c\u8a9e_\U0001f3ac");
    fs::create_directories(directory);
    test.section("Windows runtime capabilities and hardware decode");
    avb_decoder_capabilities dec_caps{};
    test.equal(avb_decoder_probe_runtime_capabilities(AVB_BACKEND_FFMPEG, "test.mp4", &dec_caps),
               AVB_OK, "probe decoder runtime");
    for (auto device : {AVB_HW_DEVICE_D3D11VA, AVB_HW_DEVICE_CUDA, AVB_HW_DEVICE_QSV}) {
        const bool native = decode(test, argv[1], device, AVB_VIDEO_MEMORY_BACKEND_NATIVE, true);
        if (!native) continue;
        std::printf("hardware decode device %d: native and CPU readback\n", device);
        test.check(contains(dec_caps.hardware_devices, dec_caps.hardware_device_count, device),
                   "working decoder device appears in runtime capabilities");
        test.check(contains(dec_caps.video_memory, dec_caps.video_memory_count, AVB_VIDEO_MEMORY_BACKEND_NATIVE),
                   "working native decode appears in runtime capabilities");
        test.check(decode(test, argv[1], device, AVB_VIDEO_MEMORY_CPU, true),
                   "hardware decoder supports CPU readback");
    }
    test.section("Windows encoding policies and UTF-8 paths");
    avb_encoder_capabilities enc_caps{};
    test.equal(avb_encoder_probe_runtime_capabilities(AVB_BACKEND_FFMPEG, "test.mp4", &enc_caps),
               AVB_OK, "probe encoder runtime");
    bool any_hardware = false;
    bool amf_available = false;
    for (auto device : {AVB_HW_DEVICE_AMF, AVB_HW_DEVICE_D3D11VA, AVB_HW_DEVICE_CUDA}) {
        const auto suffix = std::to_string(static_cast<int>(device));
        const bool advertised = contains(enc_caps.hardware_devices, enc_caps.hardware_device_count, device);
        const bool opened = encode(test, directory / ("require_" + suffix + ".mp4"),
                                   device, AVB_HARDWARE_REQUIRE,
                                   advertised || (device == AVB_HW_DEVICE_D3D11VA && amf_available));
        if (device == AVB_HW_DEVICE_AMF) amf_available = opened;
        if (opened) {
            any_hardware = true;
            test.check(advertised, "working encoder device appears in runtime capabilities");
        }
        // Must also work when the registered hardware encoder cannot initialize.
        encode(test, directory / ("prefer_" + suffix + ".mp4"), device, AVB_HARDWARE_PREFER, true);
    }
    encode(test, directory / "auto_require.mp4", AVB_HW_DEVICE_AUTO, AVB_HARDWARE_REQUIRE, any_hardware);
    encode(test, directory / "auto_prefer.mp4", AVB_HW_DEVICE_AUTO, AVB_HARDWARE_PREFER, true);
    return test.finish("FFmpeg Windows");
}
