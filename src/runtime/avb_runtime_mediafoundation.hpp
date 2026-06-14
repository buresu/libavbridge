#pragma once

#include "avb_capability_common.hpp"

#if defined(AVB_ENABLE_MEDIAFOUNDATION)

bool avb_probe_mediafoundation_decoder(
    avb_decoder_capabilities &out,
    avb::detail::Container container);

bool avb_probe_mediafoundation_encoder(
    avb_encoder_capabilities &out,
    avb::detail::Container container);

#endif
