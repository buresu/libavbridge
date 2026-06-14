#pragma once

#include "avb_media_rules.hpp"

namespace avb::validation {

struct EncoderContext {
    detail::Container container;
    avb_video_codec video_codec;
    avb_audio_codec audio_codec;
    bool custom_video;
};

bool validate_encoder_backend(
    const avb_encode_options &options,
    const EncoderContext &context,
    avb_encoder_validation &out);

}  // namespace avb::validation
