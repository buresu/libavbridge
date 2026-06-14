#pragma once

#include "avb_media_rules.hpp"

void avb_fill_gstreamer_capabilities(
    avb_decoder_capabilities &out,
    avb::detail::Container container);

void avb_fill_gstreamer_capabilities(
    avb_encoder_capabilities &out,
    avb::detail::Container container);
