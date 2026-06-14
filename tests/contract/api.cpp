#include "test.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace {

using avb::test::Context;

constexpr std::array<avb_result, 11> results = {
    AVB_OK,
    AVB_ERROR_UNKNOWN,
    AVB_ERROR_INVALID_ARGUMENT,
    AVB_ERROR_BACKEND_NOT_AVAILABLE,
    AVB_ERROR_OPEN_FAILED,
    AVB_ERROR_STREAM_NOT_FOUND,
    AVB_ERROR_DECODE_FAILED,
    AVB_ERROR_SEEK_FAILED,
    AVB_ERROR_EOF,
    AVB_ERROR_ENCODE_FAILED,
    AVB_ERROR_AGAIN,
};

int reject_decode(const avb_video_stream_info *, const avb_decode_options *) {
  return 0;
}

avb_result open_decoder(void **, const avb_video_stream_info *,
                        const avb_decode_options *) {
  return AVB_ERROR_OPEN_FAILED;
}

avb_result decode_packet(void *, const avb_encoded_packet *,
                         avb_video_frame *) {
  return AVB_ERROR_DECODE_FAILED;
}

int reject_encode(const avb_video_encode_info *) { return 0; }

avb_result open_encoder(void **, const avb_video_encode_info *,
                        avb_encoded_video_stream *) {
  return AVB_ERROR_OPEN_FAILED;
}

avb_result encode_frame(void *, const avb_video_frame *, double,
                        avb_encoded_packet *) {
  return AVB_ERROR_ENCODE_FAILED;
}

void check_names(Context &test) {
  test.section("names");

  const std::string expected_version = std::to_string(AVB_VERSION_MAJOR) + "." +
                                       std::to_string(AVB_VERSION_MINOR) + "." +
                                       std::to_string(AVB_VERSION_PATCH);
  test.string(avb_version_string(), expected_version.c_str(),
              "version string matches public version macros");

  for (int value = AVB_BACKEND_AUTO; value < AVB_BACKEND_COUNT; ++value) {
    const auto backend = static_cast<avb_backend>(value);
    const char *name = avb_backend_name(backend);
    avb_backend parsed = AVB_BACKEND_COUNT;
    test.check(name != nullptr, "every backend has a name");
    test.check(name && avb_backend_from_name(name, &parsed) == AVB_OK &&
                   parsed == backend,
               "backend names round-trip");
  }
  test.check(avb_backend_name(static_cast<avb_backend>(-1)) == nullptr,
             "negative backend has no name");
  test.check(avb_backend_name(AVB_BACKEND_COUNT) == nullptr,
             "backend sentinel has no name");
  test.equal(avb_backend_from_name(nullptr, nullptr),
             AVB_ERROR_INVALID_ARGUMENT,
             "backend parser rejects null arguments");
  avb_backend parsed_backend = AVB_BACKEND_AUTO;
  test.equal(avb_backend_from_name("FFMPEG", &parsed_backend),
             AVB_ERROR_INVALID_ARGUMENT, "backend parser is case-sensitive");
  test.equal(avb_backend_from_name("ffmpeg", nullptr),
             AVB_ERROR_INVALID_ARGUMENT, "backend parser requires output");
  test.check(avb_backend_is_available(AVB_BACKEND_COUNT) == 0,
             "invalid backend is unavailable");

  for (int value = AVB_VIDEO_CODEC_AUTO; value < AVB_VIDEO_CODEC_COUNT;
       ++value) {
    const auto codec = static_cast<avb_video_codec>(value);
    const char *name = avb_video_codec_name(codec);
    avb_video_codec parsed = AVB_VIDEO_CODEC_COUNT;
    test.check(name != nullptr, "every video codec has a name");
    test.check(name && avb_video_codec_from_name(name, &parsed) == AVB_OK &&
                   parsed == codec,
               "video codec names round-trip");
  }
  test.check(avb_video_codec_name(AVB_VIDEO_CODEC_COUNT) == nullptr,
             "video codec sentinel has no name");
  avb_video_codec parsed_video = AVB_VIDEO_CODEC_AUTO;
  test.equal(avb_video_codec_from_name("aac", &parsed_video),
             AVB_ERROR_INVALID_ARGUMENT,
             "video codec parser rejects audio names");
  test.equal(avb_video_codec_from_name("h264", nullptr),
             AVB_ERROR_INVALID_ARGUMENT, "video codec parser requires output");

  for (int value = AVB_AUDIO_CODEC_AUTO; value < AVB_AUDIO_CODEC_COUNT;
       ++value) {
    const auto codec = static_cast<avb_audio_codec>(value);
    const char *name = avb_audio_codec_name(codec);
    avb_audio_codec parsed = AVB_AUDIO_CODEC_COUNT;
    test.check(name != nullptr, "every audio codec has a name");
    test.check(name && avb_audio_codec_from_name(name, &parsed) == AVB_OK &&
                   parsed == codec,
               "audio codec names round-trip");
  }
  test.check(avb_audio_codec_name(AVB_AUDIO_CODEC_COUNT) == nullptr,
             "audio codec sentinel has no name");
  avb_audio_codec parsed_audio = AVB_AUDIO_CODEC_AUTO;
  test.equal(avb_audio_codec_from_name("h264", &parsed_audio),
             AVB_ERROR_INVALID_ARGUMENT,
             "audio codec parser rejects video names");
  test.equal(avb_audio_codec_from_name("aac", nullptr),
             AVB_ERROR_INVALID_ARGUMENT, "audio codec parser requires output");

  for (avb_result result : results) {
    const char *name = avb_result_string(result);
    test.check(name && name[0] != '\0',
               "every public result has a non-empty name");
  }
  test.string(avb_result_string(static_cast<avb_result>(12345)),
              "AVB_ERROR_UNKNOWN", "unknown result maps to AVB_ERROR_UNKNOWN");
}

void check_defaults(Context &test) {
  test.section("defaults");

  test.equal(AVB_COLOR_RANGE_UNKNOWN, 0,
             "unknown color range remains zero-initializable");
  test.equal(AVB_COLOR_RANGE_LIMITED, 1,
             "limited color range has a stable ABI value");
  test.equal(AVB_COLOR_RANGE_FULL, 2,
             "full color range has a stable ABI value");
  test.equal(AVB_COLOR_MATRIX_UNKNOWN, 0,
             "unknown color matrix remains zero-initializable");
  test.equal(AVB_COLOR_MATRIX_BT601, 1,
             "BT.601 color matrix has a stable ABI value");
  test.equal(AVB_COLOR_MATRIX_BT709, 2,
             "BT.709 color matrix has a stable ABI value");
  test.equal(AVB_COLOR_MATRIX_BT2020_NCL, 3,
             "BT.2020 NCL color matrix has a stable ABI value");

  const avb_decode_options decode = avb_decode_options_default();
  test.equal(decode.backend, AVB_BACKEND_AUTO, "decode defaults to AUTO");
  test.equal(decode.audio_stream_index, -1, "audio stream defaults to auto");
  test.equal(decode.video_stream_index, -1, "video stream defaults to auto");
  test.equal(decode.enable_audio, 1, "audio decode is enabled by default");
  test.equal(decode.enable_video, 1, "video decode is enabled by default");
  test.equal(decode.video_format, AVB_PIXEL_FORMAT_UNKNOWN,
             "decode pixel format defaults to backend choice");
  test.equal(decode.video_memory, AVB_VIDEO_MEMORY_CPU,
             "decode memory defaults to CPU");
  test.equal(decode.video_external_type, AVB_VIDEO_EXTERNAL_NONE,
             "decode external type defaults to none");
  test.equal(decode.hardware_policy, AVB_HARDWARE_DISABLED,
             "decode hardware defaults to disabled");
  test.equal(decode.hardware_device, AVB_HW_DEVICE_AUTO,
             "decode hardware device defaults to auto");
  test.equal(decode.audio_sample_rate, 0, "decode keeps source sample rate");
  test.equal(decode.audio_channels, 0, "decode keeps source channels");
  test.equal(decode.enable_custom_video_decoders, 1,
             "custom video decoders are enabled by default");
  test.check(decode.hardware_context == nullptr,
             "decode hardware context defaults to null");

  const avb_encode_options encode = avb_encode_options_default();
  test.equal(encode.backend, AVB_BACKEND_AUTO, "encode defaults to AUTO");
  test.equal(encode.video.enable, 0, "video encode is disabled by default");
  test.equal(encode.audio.enable, 0, "audio encode is disabled by default");
  test.equal(encode.video.codec, AVB_VIDEO_CODEC_AUTO,
             "video codec defaults to AUTO");
  test.equal(encode.audio.codec, AVB_AUDIO_CODEC_AUTO,
             "audio codec defaults to AUTO");
  test.equal(encode.video.input_memory, AVB_VIDEO_MEMORY_CPU,
             "encoder input memory defaults to CPU");
  test.equal(encode.video.input_external_type, AVB_VIDEO_EXTERNAL_NONE,
             "encoder external type defaults to none");
  test.equal(encode.video.hardware_policy, AVB_HARDWARE_DISABLED,
             "encoder hardware defaults to disabled");
  test.equal(encode.video.hardware_device, AVB_HW_DEVICE_AUTO,
             "encoder hardware device defaults to auto");
  test.check(encode.video.hardware_context == nullptr,
             "encoder hardware context defaults to null");
}

void expect_decoder_validation(Context &test, const avb_decode_options &options,
                               avb_result expected, const char *message) {
  avb_decoder_validation validation{};
  test.check(avb_decoder_validate_options(&options, &validation) == AVB_OK &&
                 validation.result == expected &&
                 validation.ok == (expected == AVB_OK) &&
                 validation.message != nullptr,
             message);
}

avb_encode_options valid_encode_options() {
  avb_encode_options options = avb_encode_options_default();
  options.video.enable = 1;
  options.video.width = 64;
  options.video.height = 48;
  options.video.frame_rate = 30.0;
  options.video.codec = AVB_VIDEO_CODEC_H264;
  options.video.input_format = AVB_PIXEL_FORMAT_BGRA8;
  return options;
}

void expect_encoder_validation(Context &test, const char *path,
                               const avb_encode_options &options,
                               avb_result expected, const char *message) {
  avb_encoder_validation validation{};
  test.check(avb_encoder_validate_options(path, &options, &validation) ==
                     AVB_OK &&
                 validation.result == expected &&
                 validation.ok == (expected == AVB_OK) &&
                 validation.message != nullptr,
             message);
}

void check_validation(Context &test) {
  test.section("validation");

  test.equal(avb_decoder_validate_options(nullptr, nullptr),
             AVB_ERROR_INVALID_ARGUMENT, "decoder validation requires output");
  avb_decoder_validation decoder_validation{};
  test.equal(avb_decoder_validate_options(nullptr, &decoder_validation), AVB_OK,
             "decoder validation accepts null options as defaults");

  avb_decode_options decode = avb_decode_options_default();
  const avb_result valid_decode_result =
      avb_backend_is_available(AVB_BACKEND_AUTO)
          ? AVB_OK
          : AVB_ERROR_BACKEND_NOT_AVAILABLE;
  expect_decoder_validation(
      test, decode, valid_decode_result,
      "default decode options have the expected availability");

  decode.enable_audio = 0;
  decode.enable_video = 0;
  expect_decoder_validation(test, decode, AVB_ERROR_INVALID_ARGUMENT,
                            "decoder requires an enabled track");
  decode = avb_decode_options_default();
  decode.audio_stream_index = -2;
  expect_decoder_validation(test, decode, AVB_ERROR_INVALID_ARGUMENT,
                            "decoder rejects invalid audio stream index");
  decode = avb_decode_options_default();
  decode.video_stream_index = -2;
  expect_decoder_validation(test, decode, AVB_ERROR_INVALID_ARGUMENT,
                            "decoder rejects invalid video stream index");
  decode = avb_decode_options_default();
  decode.audio_sample_rate = -1;
  expect_decoder_validation(test, decode, AVB_ERROR_INVALID_ARGUMENT,
                            "decoder rejects negative sample rate");
  decode = avb_decode_options_default();
  decode.audio_channels = -1;
  expect_decoder_validation(test, decode, AVB_ERROR_INVALID_ARGUMENT,
                            "decoder rejects negative channel count");
  decode = avb_decode_options_default();
  decode.video_format = static_cast<avb_pixel_format>(999);
  expect_decoder_validation(test, decode, AVB_ERROR_INVALID_ARGUMENT,
                            "decoder rejects invalid pixel format");
  decode = avb_decode_options_default();
  decode.video_memory = AVB_VIDEO_MEMORY_EXTERNAL;
  expect_decoder_validation(
      test, decode, AVB_ERROR_INVALID_ARGUMENT,
      "decoder rejects external memory without an external type");
  decode = avb_decode_options_default();
  decode.video_external_type = AVB_VIDEO_EXTERNAL_DMABUF;
  expect_decoder_validation(test, decode, AVB_ERROR_INVALID_ARGUMENT,
                            "decoder rejects external type with CPU memory");
  decode = avb_decode_options_default();
  decode.video_memory = AVB_VIDEO_MEMORY_BACKEND_NATIVE;
  expect_decoder_validation(
      test, decode, AVB_ERROR_INVALID_ARGUMENT,
      "decoder requires hardware policy for native memory");
  decode = avb_decode_options_default();
  decode.hardware_policy = static_cast<avb_hardware_policy>(999);
  expect_decoder_validation(test, decode, AVB_ERROR_INVALID_ARGUMENT,
                            "decoder rejects invalid hardware policy");
  decode = avb_decode_options_default();
  decode.hardware_device = static_cast<avb_hardware_device>(999);
  expect_decoder_validation(test, decode, AVB_ERROR_INVALID_ARGUMENT,
                            "decoder rejects invalid hardware device");

  avb_encoder_validation encoder_validation{};
  avb_encode_options encode = valid_encode_options();
  test.equal(
      avb_encoder_validate_options(nullptr, &encode, &encoder_validation),
      AVB_ERROR_INVALID_ARGUMENT, "encoder validation requires a path");
  test.equal(
      avb_encoder_validate_options("out.mp4", nullptr, &encoder_validation),
      AVB_ERROR_INVALID_ARGUMENT, "encoder validation requires options");
  test.equal(avb_encoder_validate_options("out.mp4", &encode, nullptr),
             AVB_ERROR_INVALID_ARGUMENT, "encoder validation requires output");

  const avb_result valid_encode_result =
      avb_backend_is_available(AVB_BACKEND_AUTO)
          ? AVB_OK
          : AVB_ERROR_BACKEND_NOT_AVAILABLE;
  expect_encoder_validation(
      test, "out.mp4", encode, valid_encode_result,
      "valid H.264 MP4 options have the expected availability");

  encode = avb_encode_options_default();
  expect_encoder_validation(test, "out.mp4", encode, AVB_ERROR_INVALID_ARGUMENT,
                            "encoder requires an enabled track");
  encode = valid_encode_options();
  encode.video.width = 0;
  expect_encoder_validation(test, "out.mp4", encode, AVB_ERROR_INVALID_ARGUMENT,
                            "encoder rejects zero video width");
  encode = valid_encode_options();
  encode.video.input_format = static_cast<avb_pixel_format>(999);
  expect_encoder_validation(test, "out.mp4", encode, AVB_ERROR_INVALID_ARGUMENT,
                            "encoder rejects invalid input format");
  encode = valid_encode_options();
  encode.video.input_memory = AVB_VIDEO_MEMORY_EXTERNAL;
  expect_encoder_validation(
      test, "out.mp4", encode, AVB_ERROR_INVALID_ARGUMENT,
      "encoder rejects external memory without external type");
  encode = valid_encode_options();
  encode.video.hardware_policy = static_cast<avb_hardware_policy>(999);
  expect_encoder_validation(test, "out.mp4", encode, AVB_ERROR_INVALID_ARGUMENT,
                            "encoder rejects invalid hardware policy");
  encode = valid_encode_options();
  encode.video.hardware_device = static_cast<avb_hardware_device>(999);
  expect_encoder_validation(test, "out.mp4", encode, AVB_ERROR_INVALID_ARGUMENT,
                            "encoder rejects invalid hardware device");
  encode = valid_encode_options();
  encode.video.codec = AVB_VIDEO_CODEC_HAP;
  expect_encoder_validation(test, "out.mov", encode, AVB_ERROR_INVALID_ARGUMENT,
                            "HAP requires a custom encoder");
  encode = valid_encode_options();
  encode.audio.enable = 1;
  encode.audio.sample_rate = 48000;
  encode.audio.channels = 2;
  encode.audio.codec = AVB_AUDIO_CODEC_OPUS;
  expect_encoder_validation(test, "out.mp4", encode, AVB_ERROR_INVALID_ARGUMENT,
                            "MP4 rejects Opus");
  encode = valid_encode_options();
  expect_encoder_validation(test, "out.wav", encode, AVB_ERROR_INVALID_ARGUMENT,
                            "audio-only containers reject video");
}

template <typename Capabilities>
void check_common_capability_shape(Context &test, const Capabilities &caps,
                                   const char *expected_container) {
  test.check(caps.container_name != nullptr, "capability container is set");
  test.string(caps.container_name, expected_container,
              "container inference matches extension");
  test.check(caps.message != nullptr, "capability message is set");
  if (caps.result != AVB_OK) {
    test.equal(caps.result, AVB_ERROR_BACKEND_NOT_AVAILABLE,
               "unusable capability query reports backend unavailable");
    return;
  }
  test.check(caps.backend_name != nullptr, "capability backend name is set");
  test.check(avb::test::unique(caps.video_codecs, caps.video_codec_count),
             "video codec capabilities are bounded and unique");
  test.check(avb::test::unique(caps.audio_codecs, caps.audio_codec_count),
             "audio codec capabilities are bounded and unique");
  test.check(avb::test::unique(caps.video_memory, caps.video_memory_count),
             "memory capabilities are bounded and unique");
  test.check(avb::test::unique(caps.video_external_types,
                               caps.video_external_type_count),
             "external type capabilities are bounded and unique");
  test.check(
      avb::test::unique(caps.hardware_devices, caps.hardware_device_count),
      "hardware device capabilities are bounded and unique");
  test.check(avb::test::contains(caps.video_memory, caps.video_memory_count,
                                 AVB_VIDEO_MEMORY_EXTERNAL) ==
                 (caps.video_external_type_count > 0),
             "external memory and external type capabilities agree");
}

void check_decoder_capability_shape(Context &test,
                                    const avb_decoder_capabilities &caps,
                                    const char *expected_container) {
  check_common_capability_shape(test, caps, expected_container);
  if (caps.result != AVB_OK)
    return;
  test.check(caps.can_decode_video == (caps.video_codec_count > 0),
             "decoder video flag matches codec count");
  test.check(caps.can_decode_audio == (caps.audio_codec_count > 0),
             "decoder audio flag matches codec count");
  test.check(avb::test::unique(caps.pixel_formats, caps.pixel_format_count),
             "pixel format capabilities are bounded and unique");
}

void check_encoder_capability_shape(Context &test,
                                    const avb_encoder_capabilities &caps,
                                    const char *expected_container) {
  check_common_capability_shape(test, caps, expected_container);
  if (caps.result != AVB_OK)
    return;
  test.check(caps.can_encode_video == (caps.video_codec_count > 0),
             "encoder video flag matches codec count");
  test.check(caps.can_encode_audio == (caps.audio_codec_count > 0),
             "encoder audio flag matches codec count");
}

void check_capabilities(Context &test) {
  test.section("capabilities");

  test.equal(avb_decoder_query_capabilities(AVB_BACKEND_AUTO, nullptr, nullptr),
             AVB_ERROR_INVALID_ARGUMENT,
             "decoder static query requires output");
  test.equal(avb_decoder_probe_runtime_capabilities(AVB_BACKEND_AUTO, nullptr,
                                                    nullptr),
             AVB_ERROR_INVALID_ARGUMENT,
             "decoder runtime query requires output");
  test.equal(avb_encoder_query_capabilities(AVB_BACKEND_AUTO, nullptr, nullptr),
             AVB_ERROR_INVALID_ARGUMENT,
             "encoder static query requires output");
  test.equal(avb_encoder_probe_runtime_capabilities(AVB_BACKEND_AUTO, nullptr,
                                                    nullptr),
             AVB_ERROR_INVALID_ARGUMENT,
             "encoder runtime query requires output");

  struct PathCase {
    const char *path;
    const char *container;
  };
  constexpr PathCase paths[] = {
      {nullptr, "any"},     {"input.MP4", "mp4"},   {"input.mov", "mov"},
      {"input.m4a", "m4a"}, {"input.WEBM", "webm"}, {"input.mkv", "mkv"},
      {"input.ogg", "ogg"}, {"input.wav", "wav"},   {"input.flac", "flac"},
      {"input.mp3", "mp3"}, {"input.ivf", "ivf"},   {"input.unknown", "any"},
  };

  for (int value = AVB_BACKEND_AUTO; value < AVB_BACKEND_COUNT; ++value) {
    const auto backend = static_cast<avb_backend>(value);
    for (const PathCase &path : paths) {
      avb_decoder_capabilities decoder{};
      test.equal(avb_decoder_query_capabilities(backend, path.path, &decoder),
                 AVB_OK, "decoder static capability query runs");
      check_decoder_capability_shape(test, decoder, path.container);

      avb_encoder_capabilities encoder{};
      test.equal(avb_encoder_query_capabilities(backend, path.path, &encoder),
                 AVB_OK, "encoder static capability query runs");
      check_encoder_capability_shape(test, encoder, path.container);
    }

    avb_decoder_capabilities decoder_runtime{};
    test.equal(avb_decoder_probe_runtime_capabilities(backend, nullptr,
                                                      &decoder_runtime),
               AVB_OK, "decoder runtime capability query runs");
    check_decoder_capability_shape(test, decoder_runtime, "any");

    avb_encoder_capabilities encoder_runtime{};
    test.equal(avb_encoder_probe_runtime_capabilities(backend, nullptr,
                                                      &encoder_runtime),
               AVB_OK, "encoder runtime capability query runs");
    check_encoder_capability_shape(test, encoder_runtime, "any");
  }
}

void check_plugins(Context &test) {
  test.section("plugin registry");

  test.equal(avb_register_video_decoder(nullptr), AVB_ERROR_INVALID_ARGUMENT,
             "decoder registry rejects null");
  avb_video_decoder_plugin decoder{};
  test.equal(avb_register_video_decoder(&decoder), AVB_ERROR_INVALID_ARGUMENT,
             "decoder registry rejects empty plugin");
  decoder.struct_size = sizeof(decoder) - 1;
  decoder.can_decode = reject_decode;
  decoder.open = open_decoder;
  decoder.decode_packet = decode_packet;
  test.equal(avb_register_video_decoder(&decoder), AVB_ERROR_INVALID_ARGUMENT,
             "decoder registry rejects old struct size");
  decoder.struct_size = sizeof(decoder);
  test.equal(avb_register_video_decoder(&decoder), AVB_OK,
             "decoder registry accepts complete plugin");
  test.equal(avb_register_video_decoder(&decoder), AVB_OK,
             "decoder registration is idempotent");
  test.equal(avb_unregister_video_decoder(&decoder), AVB_OK,
             "decoder plugin unregisters");
  test.equal(avb_unregister_video_decoder(&decoder), AVB_ERROR_INVALID_ARGUMENT,
             "decoder plugin cannot unregister twice");

  test.equal(avb_register_video_encoder(nullptr), AVB_ERROR_INVALID_ARGUMENT,
             "encoder registry rejects null");
  avb_video_encoder_plugin encoder{};
  test.equal(avb_register_video_encoder(&encoder), AVB_ERROR_INVALID_ARGUMENT,
             "encoder registry rejects empty plugin");
  encoder.struct_size = sizeof(encoder) - 1;
  encoder.can_encode = reject_encode;
  encoder.open = open_encoder;
  encoder.encode_frame = encode_frame;
  test.equal(avb_register_video_encoder(&encoder), AVB_ERROR_INVALID_ARGUMENT,
             "encoder registry rejects old struct size");
  encoder.struct_size = sizeof(encoder);
  test.equal(avb_register_video_encoder(&encoder), AVB_OK,
             "encoder registry accepts complete plugin");
  test.equal(avb_register_video_encoder(&encoder), AVB_OK,
             "encoder registration is idempotent");
  test.equal(avb_unregister_video_encoder(&encoder), AVB_OK,
             "encoder plugin unregisters");
  test.equal(avb_unregister_video_encoder(&encoder), AVB_ERROR_INVALID_ARGUMENT,
             "encoder plugin cannot unregister twice");
}

void check_null_safety(Context &test) {
  test.section("null safety");

  avb_decode_options decode = avb_decode_options_default();
  avb_decoder *decoder = nullptr;
  avb_io_callbacks callbacks{};
  unsigned char byte = 0;

  test.equal(avb_decoder_open(nullptr, "missing.mp4", &decode),
             AVB_ERROR_INVALID_ARGUMENT, "decoder open requires output");
  test.equal(avb_decoder_open(&decoder, nullptr, &decode),
             AVB_ERROR_INVALID_ARGUMENT, "decoder open requires path");
  test.equal(avb_decoder_open_io(nullptr, &callbacks, nullptr, &decode),
             AVB_ERROR_INVALID_ARGUMENT, "decoder open_io requires output");
  test.equal(avb_decoder_open_io(&decoder, nullptr, nullptr, &decode),
             AVB_ERROR_INVALID_ARGUMENT, "decoder open_io requires callbacks");
  test.equal(avb_decoder_open_io(&decoder, &callbacks, nullptr, &decode),
             AVB_ERROR_INVALID_ARGUMENT,
             "decoder open_io requires read callback");
  test.equal(avb_decoder_open_memory(nullptr, &byte, 1, &decode),
             AVB_ERROR_INVALID_ARGUMENT, "decoder open_memory requires output");
  test.equal(avb_decoder_open_memory(&decoder, nullptr, 1, &decode),
             AVB_ERROR_INVALID_ARGUMENT, "decoder open_memory requires data");
  test.equal(avb_decoder_get_media_info(nullptr, nullptr),
             AVB_ERROR_INVALID_ARGUMENT,
             "media info rejects null decoder and output");
  test.equal(avb_decoder_seek(nullptr, 0.0, nullptr),
             AVB_ERROR_INVALID_ARGUMENT, "seek rejects null decoder");
  double pts = 123.0;
  test.equal(avb_decoder_read_audio_f32(nullptr, nullptr, 0, &pts), 0,
             "audio read rejects malformed call");
  test.near(pts, -1.0, 0.0, "malformed audio read resets timestamp");
  test.equal(avb_decoder_audio_at_eof(nullptr), 0,
             "null decoder is not at audio EOF");
  test.equal(avb_decoder_read_video_frame(nullptr, nullptr),
             AVB_ERROR_INVALID_ARGUMENT,
             "video read rejects null decoder and frame");
  test.check(avb_decoder_get_last_error(nullptr) == nullptr,
             "null decoder has no error string");
  avb_decoder_release_video_frame(nullptr, nullptr);
  avb_decoder_close(nullptr);

  avb_media_probe probe{};
  test.equal(avb_probe_media(nullptr, &decode, &probe),
             AVB_ERROR_INVALID_ARGUMENT, "probe requires path");
  test.equal(avb_probe_media("missing.mp4", &decode, nullptr),
             AVB_ERROR_INVALID_ARGUMENT, "probe requires output");

  avb_encode_options encode = valid_encode_options();
  avb_encoder *encoder = nullptr;
  avb_video_frame frame{};
  float sample = 0.0f;
  test.equal(avb_encoder_open(nullptr, "out.mp4", &encode),
             AVB_ERROR_INVALID_ARGUMENT, "encoder open requires output");
  test.equal(avb_encoder_open(&encoder, nullptr, &encode),
             AVB_ERROR_INVALID_ARGUMENT, "encoder open requires path");
  test.equal(avb_encoder_open(&encoder, "out.mp4", nullptr),
             AVB_ERROR_INVALID_ARGUMENT, "encoder open requires options");
  test.equal(avb_encoder_write_video(nullptr, &frame, 0.0),
             AVB_ERROR_INVALID_ARGUMENT, "video write rejects null encoder");
  test.equal(avb_encoder_write_audio_f32(nullptr, &sample, 1),
             AVB_ERROR_INVALID_ARGUMENT, "audio write rejects null encoder");
  test.equal(avb_encoder_finish(nullptr), AVB_ERROR_INVALID_ARGUMENT,
             "finish rejects null encoder");
  test.check(avb_encoder_get_last_error(nullptr) == nullptr,
             "null encoder has no error string");
  avb_encoder_close(nullptr);
}

} // namespace

int main() {
  Context test;
  check_names(test);
  check_defaults(test);
  check_validation(test);
  check_capabilities(test);
  check_plugins(test);
  check_null_safety(test);
  return test.finish("api_contract");
}
