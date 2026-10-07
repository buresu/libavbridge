#pragma once

#include "avbridge.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// The colour range an H.264 stream declares, read from the SPS in its avcC
// configuration record (the codec_data of an MP4/MKV track).
//
// A decoder that parses the bitstream itself reports this. One that is handed
// only the container's description of the stream does not -- GStreamer's
// hardware decoders learn the colorimetry from caps, and h264parse leaves the
// range out of them -- so the backend reads the one flag it is missing.
//
// UNKNOWN when the record cannot be read or the stream states no video signal
// type, which is also what a bitstream-parsing decoder reports for it.

namespace avb::detail {

class H264BitReader {
public:
    H264BitReader(const uint8_t *data, size_t size)
        : m_data(data), m_bits(size * 8) {}

    bool ok() const { return !m_overrun; }

    uint32_t bit() {
        if (m_pos >= m_bits) {
            m_overrun = true;
            return 0;
        }
        const uint32_t value = (m_data[m_pos >> 3] >> (7 - (m_pos & 7))) & 1u;
        ++m_pos;
        return value;
    }

    uint32_t bits(int count) {
        uint32_t value = 0;
        while (count-- > 0) value = (value << 1) | bit();
        return value;
    }

    // Exp-Golomb, unsigned and signed.
    uint32_t ue() {
        int zeros = 0;
        while (bit() == 0) {
            if (m_overrun || ++zeros > 31) {
                m_overrun = true;
                return 0;
            }
        }
        return zeros == 0 ? 0 : ((1u << zeros) - 1u) + bits(zeros);
    }

    int32_t se() {
        const uint32_t code = ue();
        return (code & 1u) ? (int32_t)((code + 1u) / 2u)
                           : -(int32_t)(code / 2u);
    }

private:
    const uint8_t *m_data;
    size_t m_bits;
    size_t m_pos = 0;
    bool m_overrun = false;
};

inline void h264_skip_scaling_list(H264BitReader &reader, int size) {
    int last = 8;
    int next = 8;
    for (int i = 0; i < size; ++i) {
        if (next != 0) next = (last + reader.se() + 256) % 256;
        if (next != 0) last = next;
    }
}

// `rbsp` is an SPS payload with the NAL header and emulation prevention bytes
// already removed.
inline avb_color_range h264_sps_color_range(const uint8_t *rbsp, size_t size) {
    H264BitReader r(rbsp, size);

    const uint32_t profile_idc = r.bits(8);
    r.bits(16); // constraint flags, level_idc
    r.ue();     // seq_parameter_set_id

    switch (profile_idc) {
        case 100: case 110: case 122: case 244: case 44: case 83: case 86:
        case 118: case 128: case 138: case 139: case 134: case 135: {
            const uint32_t chroma_format_idc = r.ue();
            if (chroma_format_idc == 3) r.bit(); // separate_colour_plane_flag
            r.ue();  // bit_depth_luma_minus8
            r.ue();  // bit_depth_chroma_minus8
            r.bit(); // qpprime_y_zero_transform_bypass_flag
            if (r.bit()) { // seq_scaling_matrix_present_flag
                const int lists = chroma_format_idc != 3 ? 8 : 12;
                for (int i = 0; i < lists; ++i) {
                    if (r.bit()) h264_skip_scaling_list(r, i < 6 ? 16 : 64);
                }
            }
            break;
        }
        default:
            break;
    }

    r.ue(); // log2_max_frame_num_minus4
    const uint32_t pic_order_cnt_type = r.ue();
    if (pic_order_cnt_type == 0) {
        r.ue(); // log2_max_pic_order_cnt_lsb_minus4
    } else if (pic_order_cnt_type == 1) {
        r.bit(); // delta_pic_order_always_zero_flag
        r.se();  // offset_for_non_ref_pic
        r.se();  // offset_for_top_to_bottom_field
        const uint32_t cycle = r.ue();
        if (cycle > 255) return AVB_COLOR_RANGE_UNKNOWN;
        for (uint32_t i = 0; i < cycle; ++i) r.se();
    }
    r.ue();  // max_num_ref_frames
    r.bit(); // gaps_in_frame_num_value_allowed_flag
    r.ue();  // pic_width_in_mbs_minus1
    r.ue();  // pic_height_in_map_units_minus1
    if (!r.bit()) r.bit(); // frame_mbs_only_flag, mb_adaptive_frame_field_flag
    r.bit();               // direct_8x8_inference_flag
    if (r.bit()) {         // frame_cropping_flag
        r.ue(); r.ue(); r.ue(); r.ue();
    }
    if (!r.bit() || !r.ok()) return AVB_COLOR_RANGE_UNKNOWN; // no VUI

    if (r.bit()) { // aspect_ratio_info_present_flag
        if (r.bits(8) == 255) r.bits(32); // Extended_SAR: sar_width, sar_height
    }
    if (r.bit()) r.bit(); // overscan_info_present_flag, overscan_appropriate
    if (!r.bit() || !r.ok()) return AVB_COLOR_RANGE_UNKNOWN; // no signal type
    r.bits(3); // video_format
    const bool full_range = r.bit() != 0;
    if (!r.ok()) return AVB_COLOR_RANGE_UNKNOWN;
    return full_range ? AVB_COLOR_RANGE_FULL : AVB_COLOR_RANGE_LIMITED;
}

inline avb_color_range h264_avcc_color_range(const uint8_t *avcc, size_t size) {
    // configurationVersion, profile, compatibility, level, NAL length size,
    // SPS count, then each SPS behind a 16-bit length.
    if (!avcc || size < 8 || avcc[0] != 1 || (avcc[5] & 0x1f) == 0)
        return AVB_COLOR_RANGE_UNKNOWN;
    const size_t sps_size = ((size_t)avcc[6] << 8) | avcc[7];
    if (sps_size < 2 || sps_size > size - 8 || (avcc[8] & 0x1f) != 7)
        return AVB_COLOR_RANGE_UNKNOWN;

    // Drop the NAL header and the emulation prevention bytes (00 00 03).
    std::vector<uint8_t> rbsp;
    rbsp.reserve(sps_size);
    int zeros = 0;
    for (size_t i = 1; i < sps_size; ++i) {
        const uint8_t byte = avcc[8 + i];
        if (zeros >= 2 && byte == 3) {
            zeros = 0;
            continue;
        }
        zeros = byte == 0 ? zeros + 1 : 0;
        rbsp.push_back(byte);
    }
    return h264_sps_color_range(rbsp.data(), rbsp.size());
}

} // namespace avb::detail
