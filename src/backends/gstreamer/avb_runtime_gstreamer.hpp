#pragma once

#include "avb_media_rules.hpp"

// Runtime element and allocator discovery owned by the GStreamer backend.

#if defined(AVB_ENABLE_GSTREAMER)

bool avb_probe_gstreamer_decoder(
    avb_decoder_capabilities &out,
    avb::detail::Container container);

bool avb_probe_gstreamer_encoder(
    avb_encoder_capabilities &out,
    avb::detail::Container container);

#endif
