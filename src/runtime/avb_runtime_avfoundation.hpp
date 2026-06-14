#pragma once

#include "avb_capability_common.hpp"

#if defined(AVB_ENABLE_AVFOUNDATION)

void avb_probe_avfoundation_decoder(
    avb_decoder_capabilities &out,
    avb::detail::Container container);

void avb_probe_avfoundation_encoder(
    avb_encoder_capabilities &out,
    avb::detail::Container container);

#endif
