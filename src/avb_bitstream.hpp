#pragma once

#include "avbridge.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// Codec bitstream helpers shared by the elementary-stream encoders/decoders:
// NAL unit framing (Annex-B / length prefixes), parameter-set tracking,
// avcC/hvcC configuration records, and AAC/Opus codec configuration.

namespace avb::detail {

struct NalRef {
    const uint8_t *data = nullptr; // NAL header first, no start code / length
    size_t size = 0;
};

enum class NalFraming {
    annexb,
    length_prefixed,
};

bool is_h26x(avb_video_codec codec);

// True when the buffer starts with a 3- or 4-byte Annex-B start code.
bool starts_with_start_code(const uint8_t *data, size_t size);

// Split a packet into NAL units. Empty NALs are skipped. Returns false when a
// length-prefixed packet is truncated.
bool split_annexb(const uint8_t *data, size_t size, std::vector<NalRef> &out);
bool split_length_prefixed(const uint8_t *data, size_t size, int length_size,
                           std::vector<NalRef> &out);

void append_annexb(std::vector<uint8_t> &out, const uint8_t *nal, size_t size);
void append_length_prefixed(std::vector<uint8_t> &out, const uint8_t *nal, size_t size);

int nal_type(avb_video_codec codec, const uint8_t *nal, size_t size);

enum class ParamSetKind { none, vps, sps, pps };
ParamSetKind param_set_kind(avb_video_codec codec, int type);
// IDR (H.264) or IRAP (HEVC: BLA/IDR/CRA).
bool is_random_access_nal(avb_video_codec codec, int type);
// A slice (VCL NAL) of any kind.
bool is_vcl_nal(avb_video_codec codec, int type);

// Remove emulation prevention bytes (00 00 03) from a NAL payload.
std::vector<uint8_t> nal_to_rbsp(const uint8_t *data, size_t size);

// The most recent VPS/SPS/PPS of a stream. A packet that carries an SPS
// replaces the stored ones of every kind it carries; each kind keeps every
// distinct NAL seen in one packet (multiple PPS ids).
class ParamSets {
public:
    explicit ParamSets(avb_video_codec codec = AVB_VIDEO_CODEC_H264) : m_codec(codec) {}

    void set_codec(avb_video_codec codec) { m_codec = codec; }
    avb_video_codec codec() const { return m_codec; }

    // Record the parameter sets among `nals`. Returns true when any changed.
    bool update(const std::vector<NalRef> &nals);
    // Seed from extradata: avcC, hvcC or Annex-B parameter sets. Returns the
    // NAL length size an avcC/hvcC declares (0 when not a configuration record).
    int load_extradata(const uint8_t *data, size_t size);

    bool complete() const;
    void clear() { m_vps.clear(); m_sps.clear(); m_pps.clear(); }

    // Every stored set in decoding order (VPS, SPS, PPS).
    std::vector<const std::vector<uint8_t> *> all() const;
    const std::vector<std::vector<uint8_t>> &sps() const { return m_sps; }
    const std::vector<std::vector<uint8_t>> &pps() const { return m_pps; }
    const std::vector<std::vector<uint8_t>> &vps() const { return m_vps; }

    // avcC (H.264) or hvcC (HEVC) with 4-byte NAL lengths; empty when the
    // stored sets are incomplete or the SPS cannot be parsed.
    std::vector<uint8_t> configuration_record() const;

private:
    avb_video_codec m_codec;
    std::vector<std::vector<uint8_t>> m_vps, m_sps, m_pps;
};

// Picture size an SPS declares (after cropping). False when unparsable.
bool h264_sps_dimensions(const uint8_t *nal, size_t size, int &width, int &height);
bool hevc_sps_dimensions(const uint8_t *nal, size_t size, int &width, int &height);

// AAC-LC AudioSpecificConfig for a sample rate / channel count.
std::vector<uint8_t> aac_audio_specific_config(int sample_rate, int channels,
                                               int object_type = 2);
// Rate/channels an AudioSpecificConfig declares. False when unparsable.
bool aac_parse_audio_specific_config(const uint8_t *data, size_t size,
                                     int &sample_rate, int &channels,
                                     int &object_type);
// Length of an ADTS header at the start of `data`, 0 when there is none.
size_t aac_adts_header_size(const uint8_t *data, size_t size);
// An MPEG-4 ES descriptor (the content of an esds box) wrapping an ASC.
std::vector<uint8_t> aac_esds_from_asc(const uint8_t *asc, size_t size);
// The ASC inside an ES descriptor, empty when there is none.
std::vector<uint8_t> aac_asc_from_esds(const uint8_t *data, size_t size);

// RFC 7845 identification header ("OpusHead"), mapping family 0.
std::vector<uint8_t> opus_head(int channels, int pre_skip, int input_sample_rate);
bool opus_parse_head(const uint8_t *data, size_t size, int &channels,
                     int &pre_skip, int &input_sample_rate);

} // namespace avb::detail
