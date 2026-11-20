#include "avb_bitstream.hpp"
#include "avb_h264_sps.hpp"

#include <cstring>

namespace avb::detail {

namespace {

const int k_aac_rates[] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000,
    22050, 16000, 12000, 11025, 8000, 7350,
};

void put_be16(std::vector<uint8_t> &out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

uint32_t read_be16(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 8) | p[1];
}

struct H264SpsInfo {
    uint32_t profile_idc = 0;
    uint32_t chroma_format_idc = 1;
    uint32_t bit_depth_luma_minus8 = 0;
    uint32_t bit_depth_chroma_minus8 = 0;
    int width = 0;
    int height = 0;
};

bool parse_h264_sps(const uint8_t *nal, size_t size, H264SpsInfo &info) {
    if (!nal || size < 4) return false;
    const std::vector<uint8_t> rbsp = nal_to_rbsp(nal + 1, size - 1);
    H264BitReader r(rbsp.data(), rbsp.size());
    info.profile_idc = r.bits(8);
    r.bits(16); // constraint flags, level_idc
    r.ue();     // seq_parameter_set_id
    switch (info.profile_idc) {
        case 100: case 110: case 122: case 244: case 44: case 83: case 86:
        case 118: case 128: case 138: case 139: case 134: case 135: {
            info.chroma_format_idc = r.ue();
            if (info.chroma_format_idc == 3) r.bit();
            info.bit_depth_luma_minus8 = r.ue();
            info.bit_depth_chroma_minus8 = r.ue();
            r.bit();
            if (r.bit()) {
                const int lists = info.chroma_format_idc != 3 ? 8 : 12;
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
    const uint32_t poc_type = r.ue();
    if (poc_type == 0) {
        r.ue();
    } else if (poc_type == 1) {
        r.bit();
        r.se();
        r.se();
        const uint32_t cycle = r.ue();
        if (cycle > 255) return false;
        for (uint32_t i = 0; i < cycle; ++i) r.se();
    }
    r.ue();  // max_num_ref_frames
    r.bit(); // gaps_in_frame_num_value_allowed_flag
    const uint32_t width_mbs = r.ue() + 1;
    const uint32_t height_units = r.ue() + 1;
    const uint32_t frame_mbs_only = r.bit();
    if (!frame_mbs_only) r.bit();
    r.bit(); // direct_8x8_inference_flag
    uint32_t crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
    if (r.bit()) {
        crop_left = r.ue();
        crop_right = r.ue();
        crop_top = r.ue();
        crop_bottom = r.ue();
    }
    if (!r.ok() || width_mbs > 1024 || height_units > 1024) return false;
    const int sub_width = info.chroma_format_idc == 1 || info.chroma_format_idc == 2 ? 2 : 1;
    const int sub_height = info.chroma_format_idc == 1 ? 2 : 1;
    const int crop_x = info.chroma_format_idc == 0 ? 1 : sub_width;
    const int crop_y = (info.chroma_format_idc == 0 ? 1 : sub_height) * (2 - (int)frame_mbs_only);
    info.width = (int)width_mbs * 16 - crop_x * (int)(crop_left + crop_right);
    info.height = (2 - (int)frame_mbs_only) * (int)height_units * 16 -
                  crop_y * (int)(crop_top + crop_bottom);
    return info.width > 0 && info.height > 0;
}

struct HevcSpsInfo {
    uint8_t ptl[12] = {}; // general profile_tier_level bytes
    uint32_t max_sub_layers_minus1 = 0;
    uint32_t temporal_id_nesting = 0;
    uint32_t chroma_format_idc = 1;
    uint32_t bit_depth_luma_minus8 = 0;
    uint32_t bit_depth_chroma_minus8 = 0;
    int width = 0;
    int height = 0;
};

bool parse_hevc_sps(const uint8_t *nal, size_t size, HevcSpsInfo &info) {
    if (!nal || size < 15) return false;
    const std::vector<uint8_t> rbsp = nal_to_rbsp(nal + 2, size - 2);
    if (rbsp.size() < 13) return false;
    H264BitReader r(rbsp.data(), rbsp.size());
    r.bits(4); // sps_video_parameter_set_id
    info.max_sub_layers_minus1 = r.bits(3);
    info.temporal_id_nesting = r.bit();
    for (int i = 0; i < 12; ++i) info.ptl[i] = static_cast<uint8_t>(r.bits(8));
    const uint32_t sub = info.max_sub_layers_minus1;
    if (sub > 7) return false;
    bool profile_present[8] = {};
    bool level_present[8] = {};
    for (uint32_t i = 0; i < sub; ++i) {
        profile_present[i] = r.bit() != 0;
        level_present[i] = r.bit() != 0;
    }
    if (sub > 0) {
        for (uint32_t i = sub; i < 8; ++i) r.bits(2);
    }
    for (uint32_t i = 0; i < sub; ++i) {
        if (profile_present[i]) {
            r.bits(32);
            r.bits(32);
            r.bits(24);
        }
        if (level_present[i]) r.bits(8);
    }
    r.ue(); // sps_seq_parameter_set_id
    info.chroma_format_idc = r.ue();
    if (info.chroma_format_idc == 3) r.bit();
    const uint32_t width = r.ue();
    const uint32_t height = r.ue();
    uint32_t left = 0, right = 0, top = 0, bottom = 0;
    if (r.bit()) {
        left = r.ue();
        right = r.ue();
        top = r.ue();
        bottom = r.ue();
    }
    info.bit_depth_luma_minus8 = r.ue();
    info.bit_depth_chroma_minus8 = r.ue();
    if (!r.ok() || width == 0 || height == 0 || width > 16384 || height > 16384)
        return false;
    const int sub_width = info.chroma_format_idc == 1 || info.chroma_format_idc == 2 ? 2 : 1;
    const int sub_height = info.chroma_format_idc == 1 ? 2 : 1;
    info.width = (int)width - sub_width * (int)(left + right);
    info.height = (int)height - sub_height * (int)(top + bottom);
    return info.width > 0 && info.height > 0;
}

// Parse an MPEG-4 descriptor length (1-4 bytes, 7 bits each).
bool read_descriptor_length(const uint8_t *data, size_t size, size_t &pos, size_t &length) {
    length = 0;
    for (int i = 0; i < 4; ++i) {
        if (pos >= size) return false;
        const uint8_t byte = data[pos++];
        length = (length << 7) | (byte & 0x7f);
        if ((byte & 0x80) == 0) return true;
    }
    return true;
}

void put_descriptor(std::vector<uint8_t> &out, uint8_t tag, const std::vector<uint8_t> &body) {
    out.push_back(tag);
    // Four-byte length form, as QuickTime writes it; always valid.
    const size_t n = body.size();
    out.push_back(static_cast<uint8_t>(0x80 | ((n >> 21) & 0x7f)));
    out.push_back(static_cast<uint8_t>(0x80 | ((n >> 14) & 0x7f)));
    out.push_back(static_cast<uint8_t>(0x80 | ((n >> 7) & 0x7f)));
    out.push_back(static_cast<uint8_t>(n & 0x7f));
    out.insert(out.end(), body.begin(), body.end());
}

} // namespace

bool is_h26x(avb_video_codec codec) {
    return codec == AVB_VIDEO_CODEC_H264 || codec == AVB_VIDEO_CODEC_HEVC;
}

bool starts_with_start_code(const uint8_t *data, size_t size) {
    if (!data) return false;
    if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) return true;
    return size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1;
}

bool split_annexb(const uint8_t *data, size_t size, std::vector<NalRef> &out) {
    out.clear();
    if (!data) return size == 0;
    size_t i = 0;
    // Find the first start code.
    auto find_start = [&](size_t from, size_t &code_len) -> size_t {
        for (size_t p = from; p + 2 < size; ++p) {
            if (data[p] == 0 && data[p + 1] == 0 && data[p + 2] == 1) {
                code_len = 3;
                if (p > from && data[p - 1] == 0) {
                    code_len = 4;
                    return p - 1;
                }
                return p;
            }
        }
        code_len = 0;
        return size;
    };
    size_t code_len = 0;
    i = find_start(0, code_len);
    while (i < size) {
        const size_t begin = i + code_len;
        size_t next_len = 0;
        size_t next = find_start(begin, next_len);
        size_t end = next;
        // Trailing zero bytes belong to the next start code, not this NAL.
        while (end > begin && data[end - 1] == 0) --end;
        if (end > begin) out.push_back(NalRef{data + begin, end - begin});
        i = next;
        code_len = next_len;
    }
    return true;
}

bool split_length_prefixed(const uint8_t *data, size_t size, int length_size,
                           std::vector<NalRef> &out) {
    out.clear();
    if (length_size < 1 || length_size > 4) return false;
    size_t pos = 0;
    while (pos < size) {
        if (size - pos < (size_t)length_size) return false;
        size_t length = 0;
        for (int i = 0; i < length_size; ++i) length = (length << 8) | data[pos + i];
        pos += length_size;
        if (length > size - pos) return false;
        if (length > 0) out.push_back(NalRef{data + pos, length});
        pos += length;
    }
    return true;
}

void append_annexb(std::vector<uint8_t> &out, const uint8_t *nal, size_t size) {
    static const uint8_t code[4] = {0, 0, 0, 1};
    out.insert(out.end(), code, code + 4);
    out.insert(out.end(), nal, nal + size);
}

void append_length_prefixed(std::vector<uint8_t> &out, const uint8_t *nal, size_t size) {
    out.push_back(static_cast<uint8_t>(size >> 24));
    out.push_back(static_cast<uint8_t>(size >> 16));
    out.push_back(static_cast<uint8_t>(size >> 8));
    out.push_back(static_cast<uint8_t>(size));
    out.insert(out.end(), nal, nal + size);
}

int nal_type(avb_video_codec codec, const uint8_t *nal, size_t size) {
    if (!nal || size == 0) return -1;
    if (codec == AVB_VIDEO_CODEC_HEVC) return (nal[0] >> 1) & 0x3f;
    return nal[0] & 0x1f;
}

ParamSetKind param_set_kind(avb_video_codec codec, int type) {
    if (codec == AVB_VIDEO_CODEC_HEVC) {
        if (type == 32) return ParamSetKind::vps;
        if (type == 33) return ParamSetKind::sps;
        if (type == 34) return ParamSetKind::pps;
        return ParamSetKind::none;
    }
    if (type == 7) return ParamSetKind::sps;
    if (type == 8) return ParamSetKind::pps;
    return ParamSetKind::none;
}

bool is_random_access_nal(avb_video_codec codec, int type) {
    if (codec == AVB_VIDEO_CODEC_HEVC) return type >= 16 && type <= 23;
    return type == 5;
}

bool is_vcl_nal(avb_video_codec codec, int type) {
    if (codec == AVB_VIDEO_CODEC_HEVC) return type >= 0 && type <= 31;
    return type >= 1 && type <= 5;
}

std::vector<uint8_t> nal_to_rbsp(const uint8_t *data, size_t size) {
    std::vector<uint8_t> rbsp;
    rbsp.reserve(size);
    int zeros = 0;
    for (size_t i = 0; i < size; ++i) {
        const uint8_t byte = data[i];
        if (zeros >= 2 && byte == 3) {
            zeros = 0;
            continue;
        }
        zeros = byte == 0 ? zeros + 1 : 0;
        rbsp.push_back(byte);
    }
    return rbsp;
}

bool ParamSets::update(const std::vector<NalRef> &nals) {
    std::vector<std::vector<uint8_t>> vps, sps, pps;
    for (const NalRef &nal : nals) {
        const ParamSetKind kind = param_set_kind(m_codec, nal_type(m_codec, nal.data, nal.size));
        std::vector<std::vector<uint8_t>> *target =
            kind == ParamSetKind::vps ? &vps :
            kind == ParamSetKind::sps ? &sps :
            kind == ParamSetKind::pps ? &pps : nullptr;
        if (!target) continue;
        std::vector<uint8_t> bytes(nal.data, nal.data + nal.size);
        bool duplicate = false;
        for (const auto &existing : *target) duplicate = duplicate || existing == bytes;
        if (!duplicate) target->push_back(std::move(bytes));
    }
    bool changed = false;
    auto apply = [&](std::vector<std::vector<uint8_t>> &stored,
                     std::vector<std::vector<uint8_t>> &incoming) {
        if (incoming.empty() || stored == incoming) return;
        stored = std::move(incoming);
        changed = true;
    };
    apply(m_vps, vps);
    apply(m_sps, sps);
    apply(m_pps, pps);
    return changed;
}

int ParamSets::load_extradata(const uint8_t *data, size_t size) {
    if (!data || size == 0) return 0;
    std::vector<NalRef> nals;
    if (starts_with_start_code(data, size)) {
        split_annexb(data, size, nals);
        update(nals);
        return 0;
    }
    if (data[0] != 1) return 0;
    if (m_codec == AVB_VIDEO_CODEC_H264) {
        if (size < 7) return 0;
        const int length_size = (data[4] & 3) + 1;
        size_t pos = 5;
        for (int pass = 0; pass < 2; ++pass) {
            if (pos >= size) break;
            const int count = pass == 0 ? (data[pos] & 0x1f) : data[pos];
            ++pos;
            for (int i = 0; i < count; ++i) {
                if (pos + 2 > size) break;
                const size_t length = read_be16(data + pos);
                pos += 2;
                if (length > size - pos) break;
                nals.push_back(NalRef{data + pos, length});
                pos += length;
            }
        }
        update(nals);
        return length_size;
    }
    if (m_codec == AVB_VIDEO_CODEC_HEVC) {
        if (size < 23) return 0;
        const int length_size = (data[21] & 3) + 1;
        const int arrays = data[22];
        size_t pos = 23;
        for (int a = 0; a < arrays; ++a) {
            if (pos + 3 > size) break;
            const size_t count = read_be16(data + pos + 1);
            pos += 3;
            for (size_t i = 0; i < count; ++i) {
                if (pos + 2 > size) break;
                const size_t length = read_be16(data + pos);
                pos += 2;
                if (length > size - pos) break;
                nals.push_back(NalRef{data + pos, length});
                pos += length;
            }
        }
        update(nals);
        return length_size;
    }
    return 0;
}

bool ParamSets::complete() const {
    if (m_codec == AVB_VIDEO_CODEC_HEVC)
        return !m_vps.empty() && !m_sps.empty() && !m_pps.empty();
    return !m_sps.empty() && !m_pps.empty();
}

std::vector<const std::vector<uint8_t> *> ParamSets::all() const {
    std::vector<const std::vector<uint8_t> *> out;
    for (const auto &nal : m_vps) out.push_back(&nal);
    for (const auto &nal : m_sps) out.push_back(&nal);
    for (const auto &nal : m_pps) out.push_back(&nal);
    return out;
}

std::vector<uint8_t> ParamSets::configuration_record() const {
    std::vector<uint8_t> out;
    if (!complete()) return out;
    if (m_codec == AVB_VIDEO_CODEC_H264) {
        const std::vector<uint8_t> &sps = m_sps.front();
        H264SpsInfo info;
        if (sps.size() < 4 || !parse_h264_sps(sps.data(), sps.size(), info)) return out;
        out.push_back(1);
        out.push_back(sps[1]);
        out.push_back(sps[2]);
        out.push_back(sps[3]);
        out.push_back(0xff); // reserved + lengthSizeMinusOne = 3
        out.push_back(static_cast<uint8_t>(0xe0 | (m_sps.size() & 0x1f)));
        for (const auto &nal : m_sps) {
            put_be16(out, (uint32_t)nal.size());
            out.insert(out.end(), nal.begin(), nal.end());
        }
        out.push_back(static_cast<uint8_t>(m_pps.size()));
        for (const auto &nal : m_pps) {
            put_be16(out, (uint32_t)nal.size());
            out.insert(out.end(), nal.begin(), nal.end());
        }
        if (info.profile_idc == 100 || info.profile_idc == 110 ||
            info.profile_idc == 122 || info.profile_idc == 144) {
            out.push_back(static_cast<uint8_t>(0xfc | (info.chroma_format_idc & 3)));
            out.push_back(static_cast<uint8_t>(0xf8 | (info.bit_depth_luma_minus8 & 7)));
            out.push_back(static_cast<uint8_t>(0xf8 | (info.bit_depth_chroma_minus8 & 7)));
            out.push_back(0); // numOfSequenceParameterSetExt
        }
        return out;
    }

    HevcSpsInfo info;
    const std::vector<uint8_t> &sps = m_sps.front();
    if (!parse_hevc_sps(sps.data(), sps.size(), info)) return out;
    out.push_back(1);
    out.insert(out.end(), info.ptl, info.ptl + 12);
    put_be16(out, 0xf000);                                      // min_spatial_segmentation_idc
    out.push_back(0xfc);                                        // parallelismType
    out.push_back(static_cast<uint8_t>(0xfc | (info.chroma_format_idc & 3)));
    out.push_back(static_cast<uint8_t>(0xf8 | (info.bit_depth_luma_minus8 & 7)));
    out.push_back(static_cast<uint8_t>(0xf8 | (info.bit_depth_chroma_minus8 & 7)));
    put_be16(out, 0);                                           // avgFrameRate
    out.push_back(static_cast<uint8_t>(
        (((info.max_sub_layers_minus1 + 1) & 7) << 3) |
        ((info.temporal_id_nesting & 1) << 2) | 3));
    out.push_back(3); // VPS, SPS, PPS arrays
    auto put_array = [&](int type, const std::vector<std::vector<uint8_t>> &nals) {
        out.push_back(static_cast<uint8_t>(0x80 | type));
        put_be16(out, (uint32_t)nals.size());
        for (const auto &nal : nals) {
            put_be16(out, (uint32_t)nal.size());
            out.insert(out.end(), nal.begin(), nal.end());
        }
    };
    put_array(32, m_vps);
    put_array(33, m_sps);
    put_array(34, m_pps);
    return out;
}

bool h264_sps_dimensions(const uint8_t *nal, size_t size, int &width, int &height) {
    H264SpsInfo info;
    if (!parse_h264_sps(nal, size, info)) return false;
    width = info.width;
    height = info.height;
    return true;
}

bool hevc_sps_dimensions(const uint8_t *nal, size_t size, int &width, int &height) {
    HevcSpsInfo info;
    if (!parse_hevc_sps(nal, size, info)) return false;
    width = info.width;
    height = info.height;
    return true;
}

std::vector<uint8_t> aac_audio_specific_config(int sample_rate, int channels, int object_type) {
    int index = 15;
    for (int i = 0; i < 13; ++i) {
        if (k_aac_rates[i] == sample_rate) index = i;
    }
    const int channel_config = channels == 8 ? 7 : (channels >= 1 && channels <= 6 ? channels : 0);
    std::vector<uint8_t> out;
    if (index != 15) {
        // 5 bits object type, 4 bits rate index, 4 bits channels, 3 bits GASpecificConfig.
        const uint32_t bits = ((uint32_t)object_type << 11) | ((uint32_t)index << 7) |
                              ((uint32_t)channel_config << 3);
        put_be16(out, bits);
        return out;
    }
    // Explicit 24-bit rate: 5 + 4 + 24 + 4 + 3 = 40 bits.
    const uint64_t bits = ((uint64_t)object_type << 35) | (15ull << 31) |
                          ((uint64_t)(sample_rate & 0xffffff) << 7) |
                          ((uint64_t)channel_config << 3);
    for (int shift = 32; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(bits >> shift));
    return out;
}

bool aac_parse_audio_specific_config(const uint8_t *data, size_t size,
                                     int &sample_rate, int &channels, int &object_type) {
    if (!data || size < 2) return false;
    H264BitReader r(data, size);
    object_type = (int)r.bits(5);
    if (object_type == 31) object_type = 32 + (int)r.bits(6);
    const uint32_t index = r.bits(4);
    if (index == 15) sample_rate = (int)r.bits(24);
    else if (index < 13) sample_rate = k_aac_rates[index];
    else return false;
    const uint32_t config = r.bits(4);
    channels = config == 7 ? 8 : (int)config;
    return r.ok() && sample_rate > 0;
}

size_t aac_adts_header_size(const uint8_t *data, size_t size) {
    if (!data || size < 7) return 0;
    if (data[0] != 0xff || (data[1] & 0xf6) != 0xf0) return 0;
    const size_t header = (data[1] & 1) ? 7 : 9;
    const size_t frame_length = ((size_t)(data[3] & 3) << 11) | ((size_t)data[4] << 3) |
                                (data[5] >> 5);
    if (frame_length < header || frame_length > size) return 0;
    return header;
}

std::vector<uint8_t> aac_esds_from_asc(const uint8_t *asc, size_t size) {
    std::vector<uint8_t> specific(asc, asc + size);
    std::vector<uint8_t> decoder_config = {
        0x40,             // objectTypeIndication: MPEG-4 audio
        0x15,             // streamType audio, upStream 0, reserved 1
        0, 0, 0,          // bufferSizeDB
        0, 0, 0, 0,       // maxBitrate
        0, 0, 0, 0,       // avgBitrate
    };
    put_descriptor(decoder_config, 0x05, specific);
    std::vector<uint8_t> es = {0, 0, 0}; // ES_ID, flags
    put_descriptor(es, 0x04, decoder_config);
    put_descriptor(es, 0x06, std::vector<uint8_t>{0x02});
    std::vector<uint8_t> out;
    put_descriptor(out, 0x03, es);
    return out;
}

std::vector<uint8_t> aac_asc_from_esds(const uint8_t *data, size_t size) {
    if (!data) return {};
    // Tolerate a leading full-box version/flags word.
    size_t pos = (size > 4 && data[0] != 0x03 && data[4] == 0x03) ? 4 : 0;
    while (pos < size) {
        const uint8_t tag = data[pos++];
        size_t length = 0;
        if (!read_descriptor_length(data, size, pos, length)) return {};
        if (tag == 0x03) {
            if (pos + 3 > size) return {};
            const uint8_t flags = data[pos + 2];
            pos += 3;
            if (flags & 0x80) pos += 2;                       // dependsOn_ES_ID
            if (flags & 0x40) {                               // URL
                if (pos >= size) return {};
                pos += 1 + data[pos];
            }
            if (flags & 0x20) pos += 2;                       // OCR_ES_Id
            continue;                                         // descend
        }
        if (tag == 0x04) {
            pos += 13;
            continue;                                         // descend
        }
        if (tag == 0x05) {
            if (length > size - pos) return {};
            return std::vector<uint8_t>(data + pos, data + pos + length);
        }
        pos += length;
    }
    return {};
}

std::vector<uint8_t> opus_head(int channels, int pre_skip, int input_sample_rate) {
    std::vector<uint8_t> out = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1};
    out.push_back(static_cast<uint8_t>(channels));
    out.push_back(static_cast<uint8_t>(pre_skip));
    out.push_back(static_cast<uint8_t>(pre_skip >> 8));
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(input_sample_rate >> (8 * i)));
    out.push_back(0); // output gain
    out.push_back(0);
    out.push_back(0); // channel mapping family 0
    return out;
}

bool opus_parse_head(const uint8_t *data, size_t size, int &channels,
                     int &pre_skip, int &input_sample_rate) {
    if (!data || size < 19 || std::memcmp(data, "OpusHead", 8) != 0) return false;
    channels = data[9];
    pre_skip = data[10] | (data[11] << 8);
    input_sample_rate = (int)((uint32_t)data[12] | ((uint32_t)data[13] << 8) |
                              ((uint32_t)data[14] << 16) | ((uint32_t)data[15] << 24));
    return channels > 0;
}

} // namespace avb::detail
