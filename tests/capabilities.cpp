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

template <typename Capabilities>
bool has_memory(
    const Capabilities &caps,
    avb_video_memory_type memory) {
    for (int i = 0; i < caps.video_memory_count; ++i) {
        if (caps.video_memory[i] == memory) return true;
    }
    return false;
}

int reject_decode(
    const avb_video_stream_info *,
    const avb_decode_options *) {
    return 0;
}

avb_result open_decoder(
    void **,
    const avb_video_stream_info *,
    const avb_decode_options *) {
    return AVB_ERROR_OPEN_FAILED;
}

avb_result decode_packet(
    void *,
    const avb_encoded_packet *,
    avb_video_frame *) {
    return AVB_ERROR_DECODE_FAILED;
}

int reject_encode(const avb_video_encode_info *) {
    return 0;
}

avb_result open_encoder(
    void **,
    const avb_video_encode_info *,
    avb_encoded_video_stream *) {
    return AVB_ERROR_OPEN_FAILED;
}

avb_result encode_frame(
    void *,
    const avb_video_frame *,
    double,
    avb_encoded_packet *) {
    return AVB_ERROR_ENCODE_FAILED;
}

void check_plugin_registry() {
    avb_video_decoder_plugin decoder{};
    check(
        avb_register_video_decoder(&decoder) == AVB_ERROR_INVALID_ARGUMENT,
        "decoder registry accepted an incomplete plugin");
    decoder.struct_size = sizeof(decoder);
    decoder.can_decode = reject_decode;
    decoder.open = open_decoder;
    decoder.decode_packet = decode_packet;
    check(
        avb_register_video_decoder(&decoder) == AVB_OK &&
            avb_register_video_decoder(&decoder) == AVB_OK,
        "decoder registry rejected duplicate registration");
    check(
        avb_unregister_video_decoder(&decoder) == AVB_OK &&
            avb_unregister_video_decoder(&decoder) ==
                AVB_ERROR_INVALID_ARGUMENT,
        "decoder registry did not remove a duplicate registration once");

    avb_video_encoder_plugin encoder{};
    check(
        avb_register_video_encoder(&encoder) == AVB_ERROR_INVALID_ARGUMENT,
        "encoder registry accepted an incomplete plugin");
    encoder.struct_size = sizeof(encoder);
    encoder.can_encode = reject_encode;
    encoder.open = open_encoder;
    encoder.encode_frame = encode_frame;
    check(
        avb_register_video_encoder(&encoder) == AVB_OK &&
            avb_register_video_encoder(&encoder) == AVB_OK,
        "encoder registry rejected duplicate registration");
    check(
        avb_unregister_video_encoder(&encoder) == AVB_OK &&
            avb_unregister_video_encoder(&encoder) ==
                AVB_ERROR_INVALID_ARGUMENT,
        "encoder registry did not remove a duplicate registration once");
}

void check_capability_messages() {
    avb_decoder_capabilities decoder{};
    check(
        avb_decoder_query_capabilities(
            static_cast<avb_backend>(AVB_BACKEND_COUNT),
            nullptr,
            &decoder) == AVB_OK &&
            decoder.result == AVB_ERROR_BACKEND_NOT_AVAILABLE &&
            decoder.message &&
            std::strcmp(
                decoder.message,
                "Requested decoder backend is not available in this build.") ==
                0,
        "decoder unavailable message changed");

    avb_encoder_capabilities encoder{};
    check(
        avb_encoder_probe_runtime_capabilities(
            static_cast<avb_backend>(AVB_BACKEND_COUNT),
            nullptr,
            &encoder) == AVB_OK &&
            encoder.result == AVB_ERROR_BACKEND_NOT_AVAILABLE &&
            encoder.message &&
            std::strcmp(
                encoder.message,
                "Requested encoder backend is not available in this build.") ==
                0,
        "encoder unavailable message changed");

    if (avb_decoder_query_capabilities(
            AVB_BACKEND_AUTO, nullptr, &decoder) == AVB_OK &&
        decoder.result == AVB_OK) {
        check(
            decoder.message &&
                std::strcmp(
                decoder.message,
                "Decoder capabilities are statically available.") == 0,
            "decoder static success message changed");
    }

    if (avb_encoder_probe_runtime_capabilities(
            AVB_BACKEND_AUTO, nullptr, &encoder) == AVB_OK &&
        encoder.result == AVB_OK) {
        check(
            encoder.message &&
                std::strcmp(
                encoder.message,
                "Encoder capabilities are available in the current runtime.") ==
                0,
            "encoder runtime success message changed");
    }
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
        values_are_unique(
            caps.video_external_types, caps.video_external_type_count),
        "decoder external types contain duplicates or invalid count");
    check(
        has_memory(caps, AVB_VIDEO_MEMORY_EXTERNAL) ==
            (caps.video_external_type_count > 0),
        "decoder external memory and external type capabilities disagree");
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
        values_are_unique(
            caps.video_external_types, caps.video_external_type_count),
        "encoder external types contain duplicates or invalid count");
    check(
        has_memory(caps, AVB_VIDEO_MEMORY_EXTERNAL) ==
            (caps.video_external_type_count > 0),
        "encoder external memory and external type capabilities disagree");
    check(
        values_are_unique(caps.hardware_devices, caps.hardware_device_count),
        "encoder hardware devices contain duplicates or invalid count");
}

}  // namespace

int main() {
    check_plugin_registry();
    check_capability_messages();

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
    encode.video.input_external_type =
        static_cast<avb_video_external_type>(999);
    check(
        avb_encoder_validate_options(
            "output.mp4", &encode, &validation) == AVB_OK &&
            validation.result == AVB_ERROR_INVALID_ARGUMENT,
        "encoder validation accepted an invalid external type");
    encode.video.input_external_type = AVB_VIDEO_EXTERNAL_DMABUF;
    check(
        avb_encoder_validate_options(
            "output.mp4", &encode, &validation) == AVB_OK &&
            validation.result == AVB_ERROR_INVALID_ARGUMENT,
        "encoder validation accepted an external type with CPU memory");
    encode.video.input_external_type = AVB_VIDEO_EXTERNAL_NONE;

    avb_decode_options decode = avb_decode_options_default();
    decode.video_memory = AVB_VIDEO_MEMORY_EXTERNAL;
    avb_decoder_validation decoder_validation{};
    check(
        avb_decoder_validate_options(
            &decode, &decoder_validation) == AVB_OK &&
            decoder_validation.result == AVB_ERROR_INVALID_ARGUMENT,
        "decoder validation accepted EXTERNAL memory without an external type");
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
