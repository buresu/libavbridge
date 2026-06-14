#include "avbridge.h"

#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void check(bool condition, const char *message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

template <typename T, size_t N>
bool values_are_unique(const T (&values)[N], int count) {
    if (count < 0 || count > static_cast<int>(N)) return false;
    for (int i = 0; i < count; ++i) {
        for (int j = i + 1; j < count; ++j) {
            if (values[i] == values[j]) return false;
        }
    }
    return true;
}

void check_decoder(
    avb_result (*query)(
        avb_backend,
        const char *,
        avb_decoder_capabilities *),
    avb_backend backend,
    const char *path,
    const char *expected_container) {
    avb_decoder_capabilities caps{};
    check(
        query(backend, path, &caps) == AVB_OK,
        "decoder capability query failed");
    check(caps.container_name != nullptr, "decoder container name is null");
    check(
        caps.container_name &&
            std::strcmp(caps.container_name, expected_container) == 0,
        "decoder container inference mismatch");
    check(caps.message != nullptr, "decoder message is null");

    if (caps.result != AVB_OK) {
        check(
            caps.result == AVB_ERROR_BACKEND_NOT_AVAILABLE,
            "decoder query returned an unexpected capability result");
        return;
    }

    check(caps.backend_name != nullptr, "decoder backend name is null");
    check(
        caps.can_decode_video == (caps.video_codec_count > 0),
        "decoder video flag and codec count disagree");
    check(
        caps.can_decode_audio == (caps.audio_codec_count > 0),
        "decoder audio flag and codec count disagree");
    check(
        values_are_unique(caps.video_codecs, caps.video_codec_count),
        "decoder video codecs contain duplicates or invalid count");
    check(
        values_are_unique(caps.audio_codecs, caps.audio_codec_count),
        "decoder audio codecs contain duplicates or invalid count");
    check(
        values_are_unique(caps.pixel_formats, caps.pixel_format_count),
        "decoder pixel formats contain duplicates or invalid count");
    check(
        values_are_unique(caps.video_memory, caps.video_memory_count),
        "decoder memory types contain duplicates or invalid count");
    check(
        values_are_unique(caps.hardware_devices, caps.hardware_device_count),
        "decoder hardware devices contain duplicates or invalid count");
}

void check_encoder(
    avb_result (*query)(
        avb_backend,
        const char *,
        avb_encoder_capabilities *),
    avb_backend backend,
    const char *path,
    const char *expected_container) {
    avb_encoder_capabilities caps{};
    check(
        query(backend, path, &caps) == AVB_OK,
        "encoder capability query failed");
    check(caps.container_name != nullptr, "encoder container name is null");
    check(
        caps.container_name &&
            std::strcmp(caps.container_name, expected_container) == 0,
        "encoder container inference mismatch");
    check(caps.message != nullptr, "encoder message is null");

    if (caps.result != AVB_OK) {
        check(
            caps.result == AVB_ERROR_BACKEND_NOT_AVAILABLE,
            "encoder query returned an unexpected capability result");
        return;
    }

    check(caps.backend_name != nullptr, "encoder backend name is null");
    check(
        caps.can_encode_video == (caps.video_codec_count > 0),
        "encoder video flag and codec count disagree");
    check(
        caps.can_encode_audio == (caps.audio_codec_count > 0),
        "encoder audio flag and codec count disagree");
    check(
        values_are_unique(caps.video_codecs, caps.video_codec_count),
        "encoder video codecs contain duplicates or invalid count");
    check(
        values_are_unique(caps.audio_codecs, caps.audio_codec_count),
        "encoder audio codecs contain duplicates or invalid count");
    check(
        values_are_unique(caps.video_memory, caps.video_memory_count),
        "encoder memory types contain duplicates or invalid count");
    check(
        values_are_unique(caps.hardware_devices, caps.hardware_device_count),
        "encoder hardware devices contain duplicates or invalid count");
}

}  // namespace

int main() {
    avb_decoder_capabilities auto_caps{};
    check(
        avb_decoder_query_capabilities(
            AVB_BACKEND_AUTO, nullptr, &auto_caps) == AVB_OK,
        "AUTO backend capability query failed");
    check(
        avb_backend_is_available(AVB_BACKEND_AUTO) ==
            (auto_caps.result == AVB_OK),
        "AUTO backend availability and resolution disagree");

    check(
        avb_decoder_query_capabilities(
            AVB_BACKEND_AUTO, nullptr, nullptr) == AVB_ERROR_INVALID_ARGUMENT,
        "decoder static query accepted a null output");
    check(
        avb_decoder_probe_runtime_capabilities(
            AVB_BACKEND_AUTO, nullptr, nullptr) == AVB_ERROR_INVALID_ARGUMENT,
        "decoder runtime query accepted a null output");
    check(
        avb_encoder_query_capabilities(
            AVB_BACKEND_AUTO, nullptr, nullptr) == AVB_ERROR_INVALID_ARGUMENT,
        "encoder static query accepted a null output");
    check(
        avb_encoder_probe_runtime_capabilities(
            AVB_BACKEND_AUTO, nullptr, nullptr) == AVB_ERROR_INVALID_ARGUMENT,
        "encoder runtime query accepted a null output");

    avb_encode_options encode = avb_encode_options_default();
    encode.video.enable = 1;
    encode.video.width = 16;
    encode.video.height = 16;
    encode.video.input_format = static_cast<avb_pixel_format>(999);
    avb_encoder_validation validation{};
    check(
        avb_encoder_validate_options(
            "output.mp4", &encode, &validation) == AVB_OK &&
            validation.result == AVB_ERROR_INVALID_ARGUMENT,
        "encoder validation accepted an invalid pixel format");
    encode.video.input_format = AVB_PIXEL_FORMAT_BGRA8;
    encode.video.input_memory = static_cast<avb_video_memory_type>(999);
    check(
        avb_encoder_validate_options(
            "output.mp4", &encode, &validation) == AVB_OK &&
            validation.result == AVB_ERROR_INVALID_ARGUMENT,
        "encoder validation accepted an invalid memory type");
    encode.video.input_memory = AVB_VIDEO_MEMORY_CPU;
    encode.video.hardware_policy = static_cast<avb_hardware_policy>(999);
    check(
        avb_encoder_validate_options(
            "output.mp4", &encode, &validation) == AVB_OK &&
            validation.result == AVB_ERROR_INVALID_ARGUMENT,
        "encoder validation accepted an invalid hardware policy");
    encode.video.hardware_policy = AVB_HARDWARE_DISABLED;
    encode.video.hardware_device = static_cast<avb_hardware_device>(999);
    check(
        avb_encoder_validate_options(
            "output.mp4", &encode, &validation) == AVB_OK &&
            validation.result == AVB_ERROR_INVALID_ARGUMENT,
        "encoder validation accepted an invalid hardware device");

    const avb_backend backends[] = {
        AVB_BACKEND_AUTO,
        AVB_BACKEND_FFMPEG,
        AVB_BACKEND_GSTREAMER,
        AVB_BACKEND_MEDIAFOUNDATION,
        AVB_BACKEND_AVFOUNDATION,
    };
    for (avb_backend backend : backends) {
        check_decoder(
            avb_decoder_query_capabilities, backend, nullptr, "any");
        check_decoder(
            avb_decoder_query_capabilities,
            backend,
            "sample.WEBM",
            "webm");
        check_decoder(
            avb_decoder_probe_runtime_capabilities,
            backend,
            nullptr,
            "any");
        check_decoder(
            avb_decoder_probe_runtime_capabilities,
            backend,
            "sample.WEBM",
            "webm");
        check_encoder(
            avb_encoder_query_capabilities, backend, "output.m4a", "m4a");
        check_encoder(
            avb_encoder_query_capabilities,
            backend,
            "output.unknown",
            "any");
        check_encoder(
            avb_encoder_probe_runtime_capabilities,
            backend,
            "output.m4a",
            "m4a");
        check_encoder(
            avb_encoder_probe_runtime_capabilities,
            backend,
            "output.unknown",
            "any");
    }

    if (failures != 0) {
        std::fprintf(stderr, "%d capability API checks failed\n", failures);
        return 1;
    }
    std::printf("capability API checks passed\n");
    return 0;
}
