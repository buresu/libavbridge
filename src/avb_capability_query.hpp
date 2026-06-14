#pragma once

#include "avb_media_rules.hpp"

namespace avb::capability {

template <typename Capabilities>
struct QueryTraits;

template <>
struct QueryTraits<avb_decoder_capabilities> {
    static constexpr const char *unavailable_message =
        "Requested decoder backend is not available in this build.";
    static constexpr const char *static_success_message =
        "Decoder capabilities are statically available.";
    static constexpr const char *runtime_success_message =
        "Decoder capabilities are available in the current runtime.";

    static void finish(avb_decoder_capabilities &out) {
        out.can_decode_video = out.video_codec_count > 0 ? 1 : 0;
        out.can_decode_audio = out.audio_codec_count > 0 ? 1 : 0;
    }
};

template <>
struct QueryTraits<avb_encoder_capabilities> {
    static constexpr const char *unavailable_message =
        "Requested encoder backend is not available in this build.";
    static constexpr const char *static_success_message =
        "Encoder capabilities are statically available.";
    static constexpr const char *runtime_success_message =
        "Encoder capabilities are available in the current runtime.";

    static void finish(avb_encoder_capabilities &out) {
        out.can_encode_video = out.video_codec_count > 0 ? 1 : 0;
        out.can_encode_audio = out.audio_codec_count > 0 ? 1 : 0;
    }
};

template <typename Capabilities, typename Provider>
avb_result run_query(
    avb_backend backend,
    const char *path,
    Capabilities *out,
    const char *success_message,
    Provider provider) {
    if (!out) return AVB_ERROR_INVALID_ARGUMENT;

    *out = {};
    detail::Container container =
        detail::container_from_path(path, detail::Container::any);
    out->result = AVB_OK;
    out->backend = detail::resolve_backend(backend);
    out->backend_name = avb_backend_name(out->backend);
    out->container_name = detail::container_name(container);

    const char *failure_message =
        QueryTraits<Capabilities>::unavailable_message;
    if (!out->backend_name || !avb_backend_is_available(out->backend) ||
        !provider(out->backend, *out, container, failure_message)) {
        out->result = AVB_ERROR_BACKEND_NOT_AVAILABLE;
        out->message = failure_message;
        return AVB_OK;
    }

    QueryTraits<Capabilities>::finish(*out);
    out->message = success_message;
    return AVB_OK;
}

}  // namespace avb::capability
