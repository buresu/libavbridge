#pragma once

#include "avb_media_rules.hpp"

// Runtime codec discovery owned by the AVFoundation backend.

#if defined(AVB_ENABLE_AVFOUNDATION)

bool avb_probe_avfoundation_decoder(
    avb_decoder_capabilities &out,
    avb::detail::Container container);

bool avb_probe_avfoundation_encoder(
    avb_encoder_capabilities &out,
    avb::detail::Container container);

#endif
