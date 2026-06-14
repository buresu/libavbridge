#include "avb_mediafoundation_ivf.hpp"

#ifdef _WIN32

#include "avb_mediafoundation_common.hpp"

#include <cstring>

namespace {

avb_video_codec codec_from_fourcc(uint32_t fourcc) {
    if (fourcc == mf_fourcc("VP80")) return AVB_VIDEO_CODEC_VP8;
    if (fourcc == mf_fourcc("VP90")) return AVB_VIDEO_CODEC_VP9;
    if (fourcc == mf_fourcc("AV01")) return AVB_VIDEO_CODEC_AV1;
    return AVB_VIDEO_CODEC_AUTO;
}

uint32_t fourcc_from_codec(avb_video_codec codec) {
    switch (codec) {
        case AVB_VIDEO_CODEC_VP8: return mf_fourcc("VP80");
        case AVB_VIDEO_CODEC_VP9: return mf_fourcc("VP90");
        case AVB_VIDEO_CODEC_AV1: return mf_fourcc("AV01");
        default: return 0;
    }
}

} // namespace

MfIvfReadResult mf_ivf_read_header(FILE *file, MfIvfHeader &out) {
    if (!file) return MfIvfReadResult::invalid;

    unsigned char bytes[32] = {};
    if (std::fread(bytes, 1, sizeof(bytes), file) != sizeof(bytes) ||
        std::memcmp(bytes, "DKIF", 4) != 0 ||
        mf_read_le16(bytes + 6) < sizeof(bytes)) {
        return MfIvfReadResult::invalid;
    }

    out.codec = codec_from_fourcc(mf_read_le32(bytes + 8));
    out.width = static_cast<int>(mf_read_le16(bytes + 12));
    out.height = static_cast<int>(mf_read_le16(bytes + 14));
    out.rate = mf_read_le32(bytes + 16);
    out.scale = mf_read_le32(bytes + 20);
    out.frame_count = mf_read_le32(bytes + 24);
    out.data_offset = static_cast<long>(mf_read_le16(bytes + 6));

    if (out.codec == AVB_VIDEO_CODEC_AUTO)
        return MfIvfReadResult::unsupported;
    if (out.width <= 0 || out.height <= 0 ||
        out.rate == 0 || out.scale == 0) {
        return MfIvfReadResult::invalid;
    }
    if (out.data_offset > static_cast<long>(sizeof(bytes)) &&
        std::fseek(file, out.data_offset, SEEK_SET) != 0) {
        return MfIvfReadResult::invalid;
    }
    return MfIvfReadResult::ok;
}

MfIvfReadResult mf_ivf_read_frame(
    FILE *file,
    std::vector<unsigned char> &packet,
    uint64_t &timestamp) {
    if (!file) return MfIvfReadResult::invalid;

    unsigned char header[12] = {};
    size_t read = std::fread(header, 1, sizeof(header), file);
    if (read != sizeof(header)) return MfIvfReadResult::eof;

    uint32_t packet_size = mf_read_le32(header);
    timestamp = mf_read_le64(header + 4);
    if (packet_size == 0 || packet_size > 256u * 1024u * 1024u)
        return MfIvfReadResult::invalid;

    packet.resize(packet_size);
    if (std::fread(packet.data(), 1, packet_size, file) != packet_size)
        return MfIvfReadResult::invalid;
    return MfIvfReadResult::ok;
}

bool mf_ivf_write_header(FILE *file, const MfIvfHeader &header) {
    uint32_t fourcc = fourcc_from_codec(header.codec);
    if (!file || !fourcc || header.width <= 0 || header.height <= 0)
        return false;

    unsigned char bytes[32] = {};
    std::memcpy(bytes, "DKIF", 4);
    mf_write_le16(bytes + 4, 0);
    mf_write_le16(bytes + 6, sizeof(bytes));
    mf_write_le32(bytes + 8, fourcc);
    mf_write_le16(bytes + 12, static_cast<uint16_t>(header.width));
    mf_write_le16(bytes + 14, static_cast<uint16_t>(header.height));
    mf_write_le32(bytes + 16, header.rate ? header.rate : 30);
    mf_write_le32(bytes + 20, header.scale ? header.scale : 1);
    mf_write_le32(bytes + 24, header.frame_count);
    return std::fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
}

bool mf_ivf_write_frame(
    FILE *file,
    const unsigned char *data,
    uint32_t size,
    uint64_t timestamp) {
    if (!file || !data || size == 0) return false;

    unsigned char header[12] = {};
    mf_write_le32(header, size);
    mf_write_le64(header + 4, timestamp);
    return std::fwrite(header, 1, sizeof(header), file) == sizeof(header) &&
           std::fwrite(data, 1, size, file) == size;
}

#endif
