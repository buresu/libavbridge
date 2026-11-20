// Fixture-free checks of the elementary-stream bitstream helpers: NAL
// framing, parameter-set tracking, avcC/hvcC records, AAC/Opus configuration.

#include "test.hpp"

#include "avb_bitstream.hpp"

#include <cstdint>
#include <vector>

using namespace avb::detail;

namespace {

class BitWriter {
public:
  void bit(int b) {
    if (m_bits % 8 == 0) m_bytes.push_back(0);
    if (b) m_bytes.back() |= (uint8_t)(0x80 >> (m_bits % 8));
    ++m_bits;
  }
  void bits(uint32_t value, int count) {
    for (int i = count - 1; i >= 0; --i) bit((value >> i) & 1);
  }
  void ue(uint32_t value) {
    const uint32_t v = value + 1;
    int len = 0;
    while ((v >> len) > 1) ++len;
    bits(0, len);
    bits(v, len + 1);
  }
  std::vector<uint8_t> finish() {
    bit(1); // rbsp_stop_one_bit
    while (m_bits % 8) bit(0);
    return m_bytes;
  }

private:
  std::vector<uint8_t> m_bytes;
  int m_bits = 0;
};

// A Baseline SPS (NAL header included) for the given macroblock grid and
// bottom crop (in luma rows / 2).
std::vector<uint8_t> h264_sps(int width_mbs, int height_mbs, int crop_bottom) {
  BitWriter w;
  w.bits(0x67, 8); // NAL header: nal_ref_idc 3, type 7
  w.bits(66, 8);   // profile_idc baseline
  w.bits(0xc0, 8); // constraint_set0/1
  w.bits(30, 8);   // level_idc
  w.ue(0);         // seq_parameter_set_id
  w.ue(0);         // log2_max_frame_num_minus4
  w.ue(2);         // pic_order_cnt_type
  w.ue(1);         // max_num_ref_frames
  w.bit(0);        // gaps_in_frame_num_value_allowed_flag
  w.ue(width_mbs - 1);
  w.ue(height_mbs - 1);
  w.bit(1); // frame_mbs_only_flag
  w.bit(1); // direct_8x8_inference_flag
  if (crop_bottom > 0) {
    w.bit(1);
    w.ue(0);
    w.ue(0);
    w.ue(0);
    w.ue((uint32_t)crop_bottom);
  } else {
    w.bit(0);
  }
  w.bit(0); // vui_parameters_present_flag
  return w.finish();
}

std::vector<uint8_t> annexb(std::initializer_list<std::vector<uint8_t>> nals) {
  std::vector<uint8_t> out;
  for (const auto &nal : nals) append_annexb(out, nal.data(), nal.size());
  return out;
}

} // namespace

int main() {
  avb::test::Context test;

  test.section("framing");
  {
    const uint8_t packet[] = {0, 0, 0, 1, 0x67, 1, 2, 0, 0, 1, 0x68, 3, 0, 0, 0, 1, 0x65, 4, 5};
    std::vector<NalRef> nals;
    test.check(split_annexb(packet, sizeof(packet), nals), "Annex-B splits");
    test.equal(nals.size(), (size_t)3, "three NAL units across 3- and 4-byte start codes");
    test.check(nals.size() == 3 && nals[0].size == 3 && nals[1].size == 2 && nals[2].size == 3,
               "NAL sizes exclude start codes");
    std::vector<uint8_t> lp;
    for (const NalRef &nal : nals) append_length_prefixed(lp, nal.data, nal.size);
    std::vector<NalRef> again;
    test.check(split_length_prefixed(lp.data(), lp.size(), 4, again) && again.size() == 3,
               "length-prefixed round trip");
    test.check(!split_length_prefixed(lp.data(), lp.size() - 1, 4, again),
               "truncated length-prefixed packet is rejected");
    test.check(starts_with_start_code(packet, sizeof(packet)), "start code detected");
    test.check(!starts_with_start_code(lp.data(), lp.size()), "length prefix is not a start code");
  }

  test.section("H.264 parameter sets");
  {
    const auto sps = h264_sps(20, 15, 0);
    int w = 0, h = 0;
    test.check(h264_sps_dimensions(sps.data(), sps.size(), w, h) && w == 320 && h == 240,
               "SPS describes 320x240");
    const auto sps1080 = h264_sps(120, 68, 4);
    test.check(h264_sps_dimensions(sps1080.data(), sps1080.size(), w, h) && w == 1920 && h == 1080,
               "cropped SPS describes 1920x1080");

    const std::vector<uint8_t> pps = {0x68, 0xce, 0x3c, 0x80};
    const std::vector<uint8_t> idr = {0x65, 0x88, 0x84, 0x00};
    const auto packet = annexb({sps, pps, idr});
    std::vector<NalRef> nals;
    split_annexb(packet.data(), packet.size(), nals);
    ParamSets ps(AVB_VIDEO_CODEC_H264);
    test.check(ps.update(nals), "first parameter sets are a change");
    test.check(!ps.update(nals), "repeated parameter sets are no change");
    test.check(ps.complete(), "SPS + PPS complete H.264");
    test.check(is_random_access_nal(AVB_VIDEO_CODEC_H264, nal_type(AVB_VIDEO_CODEC_H264, idr.data(), idr.size())),
               "IDR is random access");

    const auto avcc = ps.configuration_record();
    test.check(avcc.size() > 8 && avcc[0] == 1 && avcc[1] == 66 && (avcc[4] & 3) == 3,
               "avcC header carries profile and 4-byte lengths");
    ParamSets loaded(AVB_VIDEO_CODEC_H264);
    test.equal(loaded.load_extradata(avcc.data(), avcc.size()), 4, "avcC declares 4-byte lengths");
    test.check(loaded.sps().size() == 1 && loaded.sps()[0] == sps &&
                   loaded.pps().size() == 1 && loaded.pps()[0] == pps,
               "avcC round trip keeps SPS/PPS");
    ParamSets from_annexb(AVB_VIDEO_CODEC_H264);
    const auto sets = annexb({sps, pps});
    test.equal(from_annexb.load_extradata(sets.data(), sets.size()), 0, "Annex-B extradata has no length size");
    test.check(from_annexb.complete(), "Annex-B extradata seeds parameter sets");
  }

  test.section("AAC and Opus configuration");
  {
    const auto asc = aac_audio_specific_config(48000, 2);
    test.check(asc.size() == 2 && asc[0] == 0x11 && asc[1] == 0x90, "48 kHz stereo AAC-LC ASC is 11 90");
    int rate = 0, channels = 0, object = 0;
    test.check(aac_parse_audio_specific_config(asc.data(), asc.size(), rate, channels, object) &&
                   rate == 48000 && channels == 2 && object == 2,
               "ASC parses back");
    const auto odd = aac_audio_specific_config(37800, 1);
    test.check(aac_parse_audio_specific_config(odd.data(), odd.size(), rate, channels, object) &&
                   rate == 37800 && channels == 1,
               "explicit-rate ASC round trip");
    const auto esds = aac_esds_from_asc(asc.data(), asc.size());
    test.check(aac_asc_from_esds(esds.data(), esds.size()) == asc, "ES descriptor round trip");

    // 7-byte ADTS header (protection absent) for a 10-byte frame.
    const uint8_t adts[] = {0xff, 0xf1, 0x50, 0x80, 0x01, 0x5f, 0xfc, 1, 2, 3};
    test.equal(aac_adts_header_size(adts, sizeof(adts)), (size_t)7, "ADTS header detected");
    test.equal(aac_adts_header_size(asc.data(), asc.size()), (size_t)0, "raw AAC has no ADTS header");

    const auto head = opus_head(2, 312, 48000);
    int pre_skip = 0;
    test.check(head.size() == 19 && opus_parse_head(head.data(), head.size(), channels, pre_skip, rate) &&
                   channels == 2 && pre_skip == 312 && rate == 48000,
               "OpusHead round trip");
  }

  test.section("stream API null safety");
  {
    avb_stream_encoder *enc = nullptr;
    avb_stream_decoder *dec = nullptr;
    test.equal(avb_stream_encoder_open_video(nullptr, nullptr), AVB_ERROR_INVALID_ARGUMENT, "NULL video open");
    test.equal(avb_stream_encoder_open_audio(&enc, nullptr), AVB_ERROR_INVALID_ARGUMENT, "NULL audio params");
    test.equal(avb_stream_decoder_open(&dec, nullptr), AVB_ERROR_INVALID_ARGUMENT, "NULL decode params");
    test.equal(avb_stream_encoder_receive_packet(nullptr, nullptr), AVB_ERROR_INVALID_ARGUMENT, "NULL receive");
    test.equal(avb_stream_decoder_send_packet(nullptr, nullptr), AVB_ERROR_INVALID_ARGUMENT, "NULL send");
    test.check(avb_stream_encoder_get_last_error(nullptr) == nullptr, "NULL encoder error");
    avb_stream_encoder_close(nullptr);
    avb_stream_decoder_close(nullptr);
    const auto vp = avb_video_stream_encode_params_default();
    test.check(vp.max_b_frames == -1 && vp.repeat_parameter_sets == 1 && vp.video.enable == 1,
               "video stream defaults");
    const auto dp = avb_stream_decode_params_default();
    test.check(dp.is_video == 1 && dp.video_memory == AVB_VIDEO_MEMORY_CPU, "decode defaults");
  }

  return test.finish("bitstream contract");
}
