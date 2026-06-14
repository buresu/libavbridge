#include "avb_capabilities_gstreamer.hpp"

using avb::capability::add_portable_capabilities;
using avb::capability::add_software_pixel_formats;

void avb_fill_gstreamer_capabilities(
    avb_decoder_capabilities &out,
    avb::detail::Container container) {
    add_portable_capabilities(out, container);
    add_software_pixel_formats(out);
}

void avb_fill_gstreamer_capabilities(
    avb_encoder_capabilities &out,
    avb::detail::Container container) {
    add_portable_capabilities(out, container);
}
