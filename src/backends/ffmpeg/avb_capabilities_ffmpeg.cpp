#include "avb_capabilities_ffmpeg.hpp"

using avb::capability::add_device;
using avb::capability::add_portable_capabilities;
using avb::capability::add_software_pixel_formats;

void avb_fill_ffmpeg_capabilities(
    avb_decoder_capabilities &out,
    avb::detail::Container container) {
    add_portable_capabilities(out, container);
    add_software_pixel_formats(out);
    add_device(out, AVB_HW_DEVICE_CUDA);
    add_device(out, AVB_HW_DEVICE_QSV);
    add_device(out, AVB_HW_DEVICE_D3D11VA);
    add_device(out, AVB_HW_DEVICE_VIDEOTOOLBOX);
}

void avb_fill_ffmpeg_capabilities(
    avb_encoder_capabilities &out,
    avb::detail::Container container) {
    add_portable_capabilities(out, container);
    add_device(out, AVB_HW_DEVICE_CUDA);
    add_device(out, AVB_HW_DEVICE_QSV);
    add_device(out, AVB_HW_DEVICE_D3D11VA);
    add_device(out, AVB_HW_DEVICE_VIDEOTOOLBOX);
    add_device(out, AVB_HW_DEVICE_AMF);
}
