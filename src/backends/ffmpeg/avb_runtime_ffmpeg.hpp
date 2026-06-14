#pragma once

#include "avb_capability_common.hpp"

// Runtime codec and hardware discovery owned by the FFmpeg backend.

#if defined(AVB_ENABLE_FFMPEG)

bool avb_probe_ffmpeg_decoder(
    avb_decoder_capabilities &out,
    avb::detail::Container container);

bool avb_probe_ffmpeg_encoder(
    avb_encoder_capabilities &out,
    avb::detail::Container container);

#endif
