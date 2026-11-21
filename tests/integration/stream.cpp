// Elementary-stream round trips on one backend: encode synthetic frames and
// a sine into packets, check their framing and configuration, then decode
// them back. Codecs the backend's runtime lacks are skipped section by
// section; the test as a whole skips when not even H.264 is available.

#include "test.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int k_width = 320;
constexpr int k_height = 240;
constexpr int k_frames = 30;
constexpr double k_fps = 30.0;
constexpr double k_pi = 3.14159265358979323846;

struct Packet {
  std::vector<uint8_t> data;
  int64_t pts = 0;
  int64_t dts = 0;
  bool keyframe = false;
};

const char *stream_error(avb_stream_encoder *enc) {
  const char *e = avb_stream_encoder_get_last_error(enc);
  return e ? e : "(no error message)";
}

const char *stream_error(avb_stream_decoder *dec) {
  const char *e = avb_stream_decoder_get_last_error(dec);
  return e ? e : "(no error message)";
}

// Frame i: a fixed gradient with a grey square in the middle whose level
// rises with i, so decoded frames can be matched to their source. Only the
// square changes, as in ordinary video -- a whole picture changing every
// frame reads as a scene cut and makes encoders emit only IDR frames.
void fill_frame(std::vector<uint8_t> &pixels, int index) {
  pixels.resize((size_t)k_width * k_height * 4);
  const int level = 40 + index * 4;
  for (int y = 0; y < k_height; ++y) {
    for (int x = 0; x < k_width; ++x) {
      uint8_t *p = pixels.data() + ((size_t)y * k_width + x) * 4;
      const bool square = std::abs(x - k_width / 2) < 32 && std::abs(y - k_height / 2) < 32;
      p[0] = (uint8_t)(square ? level : 60 + x / 4);  // B
      p[1] = (uint8_t)(square ? level : 90);          // G
      p[2] = (uint8_t)(square ? level : 60 + y / 4);  // R
      p[3] = 255;
    }
  }
}

avb_video_frame bgra_frame(std::vector<uint8_t> &pixels) {
  avb_video_frame frame{};
  frame.width = k_width;
  frame.height = k_height;
  frame.format = AVB_PIXEL_FORMAT_BGRA8;
  frame.memory_type = AVB_VIDEO_MEMORY_CPU;
  frame.plane_count = 1;
  frame.plane_data[0] = frame.data = pixels.data();
  frame.plane_stride[0] = frame.stride = k_width * 4;
  frame.data_size = (int)pixels.size();
  frame.pts_sec = -1.0;
  return frame;
}

bool drain_packets(avb_stream_encoder *enc, std::vector<Packet> &out, avb::test::Context &test) {
  for (int guard = 0; guard < 100000; ++guard) {
    avb_encoded_packet pkt{};
    const avb_result r = avb_stream_encoder_receive_packet(enc, &pkt);
    if (r == AVB_ERROR_AGAIN || r == AVB_ERROR_EOF) return true;
    if (r != AVB_OK) {
      std::fprintf(stderr, "receive_packet: %s\n", stream_error(enc));
      test.check(false, "receive_packet succeeds");
      return false;
    }
    Packet p;
    p.data.assign(pkt.data, pkt.data + pkt.size);
    p.pts = pkt.pts;
    p.dts = pkt.dts;
    p.keyframe = pkt.keyframe != 0;
    out.push_back(std::move(p));
  }
  return false;
}

bool finish_encoder(avb_stream_encoder *enc, std::vector<Packet> &out, avb::test::Context &test) {
  if (avb_stream_encoder_flush(enc) != AVB_OK) {
    std::fprintf(stderr, "flush: %s\n", stream_error(enc));
    test.check(false, "flush succeeds");
    return false;
  }
  for (int guard = 0; guard < 100000; ++guard) {
    avb_encoded_packet pkt{};
    const avb_result r = avb_stream_encoder_receive_packet(enc, &pkt);
    if (r == AVB_ERROR_EOF) return true;
    if (r == AVB_ERROR_AGAIN) continue;
    if (r != AVB_OK) {
      std::fprintf(stderr, "receive_packet (flush): %s\n", stream_error(enc));
      test.check(false, "draining the encoder succeeds");
      return false;
    }
    Packet p;
    p.data.assign(pkt.data, pkt.data + pkt.size);
    p.pts = pkt.pts;
    p.dts = pkt.dts;
    p.keyframe = pkt.keyframe != 0;
    out.push_back(std::move(p));
  }
  test.check(false, "encoder reaches EOF after flush");
  return false;
}

bool has_sps(const Packet &p, bool length_prefixed) {
  size_t i = 0;
  while (i + 4 < p.data.size()) {
    if (length_prefixed) {
      const size_t n = ((size_t)p.data[i] << 24) | ((size_t)p.data[i + 1] << 16) |
                       ((size_t)p.data[i + 2] << 8) | p.data[i + 3];
      if ((p.data[i + 4] & 0x1f) == 7) return true;
      i += 4 + n;
    } else {
      if (p.data[i] == 0 && p.data[i + 1] == 0 && p.data[i + 2] == 0 && p.data[i + 3] == 1 &&
          (p.data[i + 4] & 0x1f) == 7)
        return true;
      ++i;
    }
  }
  return false;
}

struct VideoRun {
  std::vector<Packet> packets;
  std::vector<uint8_t> extradata;
  bool opened = false;
};

VideoRun encode_video(avb_backend backend, avb_bitstream_format bitstream, avb::test::Context &test,
                      avb_hardware_policy hardware = AVB_HARDWARE_DISABLED) {
  VideoRun run;
  avb_video_stream_encode_params params = avb_video_stream_encode_params_default();
  params.backend = backend;
  params.video.hardware_policy = hardware;
  params.video.width = k_width;
  params.video.height = k_height;
  params.video.frame_rate = k_fps;
  params.video.codec = AVB_VIDEO_CODEC_H264;
  params.video.bitrate = 1000000;
  params.video.input_format = AVB_PIXEL_FORMAT_BGRA8;
  params.profile = AVB_VIDEO_PROFILE_H264_CONSTRAINED_BASELINE;
  params.gop_size = 10;
  params.low_latency = 1;
  params.bitstream = bitstream;

  avb_stream_encoder *enc = nullptr;
  if (avb_stream_encoder_open_video(&enc, &params) != AVB_OK) {
    std::fprintf(stderr, "H.264 stream encoder unavailable: %s\n", stream_error(enc));
    avb_stream_encoder_close(enc);
    return run;
  }
  run.opened = true;
  std::vector<uint8_t> pixels;
  for (int i = 0; i < k_frames; ++i) {
    if (i == 15) test.equal(avb_stream_encoder_request_keyframe(enc), AVB_OK, "keyframe request accepted");
    if (i == 20) {
      const avb_result r = avb_stream_encoder_set_bitrate(enc, 800000);
      if (r != AVB_OK) std::printf("note: live bitrate change unsupported: %s\n", stream_error(enc));
    }
    fill_frame(pixels, i);
    avb_video_frame frame = bgra_frame(pixels);
    if (avb_stream_encoder_send_video(enc, &frame, i / k_fps) != AVB_OK) {
      std::fprintf(stderr, "send_video: %s\n", stream_error(enc));
      test.check(false, "send_video succeeds");
      break;
    }
    drain_packets(enc, run.packets, test);
  }
  avb_video_frame frame = bgra_frame(pixels);
  test.equal(avb_stream_encoder_send_video(enc, &frame, 0.0), AVB_ERROR_INVALID_ARGUMENT,
             "non-increasing timestamp rejected");
  finish_encoder(enc, run.packets, test);
  test.equal(avb_stream_encoder_send_video(enc, &frame, 100.0), AVB_ERROR_INVALID_ARGUMENT,
             "flushed encoder rejects frames");

  avb_stream_info info{};
  test.equal(avb_stream_encoder_get_info(enc, &info), AVB_OK, "encoder info");
  test.check(info.is_video && info.width == k_width && info.height == k_height &&
                 info.time_base_den == 90000,
             "encoder info describes the stream");
  std::printf("encoder hardware device: %d\n", (int)info.hardware_device);
  if (info.extradata && info.extradata_size > 0)
    run.extradata.assign(info.extradata, info.extradata + info.extradata_size);
  test.check(run.extradata.size() > 8 && run.extradata[0] == 1, "avcC extradata after the first keyframe");
  avb_stream_encoder_close(enc);
  return run;
}

void check_video_packets(const VideoRun &run, bool length_prefixed, avb::test::Context &test) {
  test.equal(run.packets.size(), (size_t)k_frames, "one packet per frame");
  if (run.packets.empty()) return;
  test.check(run.packets[0].keyframe, "first packet is a keyframe");
  if (std::getenv("AVB_TEST_VERBOSE")) {
    for (const Packet &p : run.packets) {
      std::printf("  pts %lld dts %lld key %d size %zu nals", (long long)p.pts, (long long)p.dts,
                  p.keyframe ? 1 : 0, p.data.size());
      for (size_t i = 0; i + 4 < p.data.size(); ++i)
        if (p.data[i] == 0 && p.data[i + 1] == 0 && p.data[i + 2] == 1) std::printf(" %d", p.data[i + 3] & 0x1f);
      std::printf("\n");
    }
  }
  bool framing = true, sps_on_keys = true, increasing = true, forced = false;
  for (size_t i = 0; i < run.packets.size(); ++i) {
    const Packet &p = run.packets[i];
    const bool start_code = p.data.size() > 4 && p.data[0] == 0 && p.data[1] == 0 &&
                            p.data[2] == 0 && p.data[3] == 1;
    framing = framing && (length_prefixed ? !start_code : start_code);
    if (p.keyframe) sps_on_keys = sps_on_keys && has_sps(p, length_prefixed);
    if (i > 0) increasing = increasing && p.pts > run.packets[i - 1].pts;
    if (p.pts == 15 * 3000 && p.keyframe) forced = true;
  }
  test.check(framing, length_prefixed ? "packets are length-prefixed" : "packets are Annex-B");
  test.check(sps_on_keys, "every keyframe carries its SPS");
  test.check(increasing, "low-latency packets come in presentation order");
  test.check(forced, "the requested frame was encoded as a keyframe");
}

void decode_video(avb_backend backend, const VideoRun &run, const std::vector<uint8_t> *extradata,
                  size_t first_packet, avb::test::Context &test, const char *label,
                  avb_hardware_policy hardware = AVB_HARDWARE_DISABLED, int low_latency = 1) {
  avb_stream_decode_params params = avb_stream_decode_params_default();
  params.backend = backend;
  params.hardware_policy = hardware;
  params.is_video = 1;
  params.video_codec = AVB_VIDEO_CODEC_H264;
  params.video_format = AVB_PIXEL_FORMAT_BGRA8;
  params.low_latency = low_latency;
  if (extradata) {
    params.extradata = extradata->data();
    params.extradata_size = (int)extradata->size();
  }
  avb_stream_decoder *dec = nullptr;
  if (avb_stream_decoder_open(&dec, &params) != AVB_OK) {
    std::fprintf(stderr, "%s: decoder open failed: %s\n", label, stream_error(dec));
    test.check(false, "H.264 stream decoder opens");
    avb_stream_decoder_close(dec);
    return;
  }
  int frames = 0;
  double first_pts = -1.0;
  bool sized = true, colour = true;
  auto take = [&]() {
    for (;;) {
      avb_video_frame frame{};
      const avb_result r = avb_stream_decoder_receive_video_frame(dec, &frame);
      if (r != AVB_OK) return r;
      sized = sized && frame.width == k_width && frame.height == k_height &&
              frame.format == AVB_PIXEL_FORMAT_BGRA8;
      if (first_pts < 0.0) first_pts = frame.pts_sec;
      // The centre pixel's green channel tracks the frame index.
      const int index = (int)std::lround(frame.pts_sec * k_fps);
      const uint8_t *centre = frame.plane_data[0] + (size_t)(k_height / 2) * frame.plane_stride[0] +
                              (size_t)(k_width / 2) * 4;
      if (std::abs((int)centre[1] - (40 + index * 4)) > 40) colour = false;
      ++frames;
      avb_stream_decoder_release_video_frame(dec, &frame);
    }
  };
  for (size_t i = first_packet; i < run.packets.size(); ++i) {
    const Packet &p = run.packets[i];
    avb_encoded_packet pkt{};
    pkt.data = p.data.data();
    pkt.size = (int)p.data.size();
    pkt.pts = p.pts;
    pkt.dts = p.dts;
    pkt.time_base_num = 1;
    pkt.time_base_den = 90000;
    pkt.keyframe = p.keyframe ? 1 : 0;
    if (avb_stream_decoder_send_packet(dec, &pkt) != AVB_OK) {
      std::fprintf(stderr, "%s: send_packet: %s\n", label, stream_error(dec));
      test.check(false, "send_packet succeeds");
      break;
    }
    const avb_result r = take();
    if (r != AVB_ERROR_AGAIN) {
      std::fprintf(stderr, "%s: receive_video_frame: %s\n", label, stream_error(dec));
      test.check(false, "receive_video_frame succeeds");
      break;
    }
  }
  test.equal(avb_stream_decoder_send_packet(dec, nullptr), AVB_OK, "decoder drain");
  const avb_result last = take();
  test.equal(last, AVB_ERROR_EOF, "decoder reaches EOF after drain");

  size_t expected = 0;
  bool seen_key = false;
  for (size_t i = first_packet; i < run.packets.size(); ++i) {
    seen_key = seen_key || run.packets[i].keyframe;
    if (seen_key) ++expected;
  }
  std::printf("%s: %d frames decoded (expected %zu)\n", label, frames, expected);
  test.equal((size_t)frames, expected, "every packet from the first keyframe decodes to a frame");
  test.check(sized, "decoded frames are 320x240 BGRA");
  test.check(colour, "decoded pixels match their source frame");
  if (first_packet > 0)
    test.check(first_pts >= 10.0 / k_fps - 1e-3, "decoding starts at the next keyframe");
  avb_stream_info info{};
  test.check(avb_stream_decoder_get_info(dec, &info) == AVB_OK && info.width == k_width,
             "decoder info reports the frame size");
  avb_stream_decoder_close(dec);
}

void audio_round_trip(avb_backend backend, avb_audio_codec codec, avb::test::Context &test) {
  const char *name = avb_audio_codec_name(codec);
  const int rate = 48000, channels = 2;
  avb_audio_stream_encode_params params = avb_audio_stream_encode_params_default();
  params.backend = backend;
  params.audio.codec = codec;
  params.audio.sample_rate = rate;
  params.audio.channels = channels;
  params.audio.bitrate = 128000;
  avb_stream_encoder *enc = nullptr;
  if (avb_stream_encoder_open_audio(&enc, &params) != AVB_OK) {
    std::printf("skip %s: %s\n", name, stream_error(enc));
    avb_stream_encoder_close(enc);
    return;
  }
  std::vector<Packet> packets;
  std::vector<float> chunk((size_t)480 * channels);
  int64_t written = 0;
  for (int block = 0; block < 100; ++block) {
    for (int i = 0; i < 480; ++i) {
      const float s = 0.5f * (float)std::sin(2.0 * k_pi * 440.0 * (double)(written + i) / rate);
      chunk[(size_t)i * channels] = chunk[(size_t)i * channels + 1] = s;
    }
    if (avb_stream_encoder_send_audio_f32(enc, chunk.data(), 480, block == 0 ? 0.0 : -1.0) != AVB_OK) {
      std::fprintf(stderr, "%s send_audio: %s\n", name, stream_error(enc));
      test.check(false, "send_audio succeeds");
      break;
    }
    written += 480;
    drain_packets(enc, packets, test);
  }
  finish_encoder(enc, packets, test);
  avb_stream_info info{};
  avb_stream_encoder_get_info(enc, &info);
  std::vector<uint8_t> extradata;
  if (info.extradata) extradata.assign(info.extradata, info.extradata + info.extradata_size);
  test.check(!packets.empty(), "audio packets produced");
  test.check(!extradata.empty(), "audio extradata available");
  if (codec == AVB_AUDIO_CODEC_AAC)
    test.check(extradata.size() >= 2 && (extradata[0] >> 3) == 2, "AAC-LC AudioSpecificConfig");
  avb_stream_encoder_close(enc);

  avb_stream_decode_params dp = avb_stream_decode_params_default();
  dp.backend = backend;
  dp.is_video = 0;
  dp.audio_codec = codec;
  dp.extradata = extradata.data();
  dp.extradata_size = (int)extradata.size();
  dp.sample_rate = rate;
  dp.channels = channels;
  avb_stream_decoder *dec = nullptr;
  if (avb_stream_decoder_open(&dec, &dp) != AVB_OK) {
    std::fprintf(stderr, "%s decoder: %s\n", name, stream_error(dec));
    test.check(false, "audio stream decoder opens");
    avb_stream_decoder_close(dec);
    return;
  }
  int64_t decoded = 0;
  double energy = 0.0;
  std::vector<float> out((size_t)4096 * channels);
  auto take = [&]() {
    for (;;) {
      int frames = 0;
      double pts = 0.0;
      const avb_result r = avb_stream_decoder_receive_audio_f32(dec, out.data(), 4096, &frames, &pts);
      if (r != AVB_OK) return r;
      for (int i = 0; i < frames * channels; ++i) energy += (double)out[i] * out[i];
      decoded += frames;
    }
  };
  for (const Packet &p : packets) {
    avb_encoded_packet pkt{};
    pkt.data = p.data.data();
    pkt.size = (int)p.data.size();
    pkt.pts = p.pts;
    pkt.time_base_num = 1;
    pkt.time_base_den = rate;
    pkt.keyframe = 1;
    if (avb_stream_decoder_send_packet(dec, &pkt) != AVB_OK) {
      std::fprintf(stderr, "%s decode: %s\n", name, stream_error(dec));
      test.check(false, "audio send_packet succeeds");
      break;
    }
    take();
  }
  avb_stream_decoder_send_packet(dec, nullptr);
  take();
  const double rms = decoded > 0 ? std::sqrt(energy / ((double)decoded * channels)) : 0.0;
  std::printf("%s: %lld of %lld samples decoded, rms %.3f\n", name, (long long)decoded,
              (long long)written, rms);
  test.check(decoded >= written - 2048 && decoded <= written + 4096, "decoded length matches the input");
  test.check(rms > 0.2 && rms < 0.5, "decoded sine keeps its level");
  avb_stream_decoder_close(dec);
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  avb_backend backend;
  if (!avb::test::parse_backend(argv[1], backend)) return 2;
  if (!avb::test::backend_is_built(backend)) return avb::test::skip;
  avb::test::Context test;

  test.section("H.264 Annex-B encode");
  const VideoRun annexb = encode_video(backend, AVB_BITSTREAM_ANNEXB, test);
  if (!annexb.opened) return avb::test::skip;
  check_video_packets(annexb, false, test);

  test.section("H.264 decode");
  decode_video(backend, annexb, nullptr, 0, test, "annexb");

  test.section("H.264 decode joining mid-stream");
  decode_video(backend, annexb, nullptr, 1, test, "join");

  test.section("H.264 length-prefixed with avcC");
  const VideoRun prefixed = encode_video(backend, AVB_BITSTREAM_LENGTH_PREFIXED, test);
  if (prefixed.opened) {
    check_video_packets(prefixed, true, test);
    decode_video(backend, prefixed, &prefixed.extradata, 0, test, "avcc");
  }

  // PREFER falls back to software where there is no hardware codec, so this
  // holds everywhere; on a GPU machine it exercises the asynchronous paths.
  test.section("H.264 hardware preferred");
  const VideoRun hardware = encode_video(backend, AVB_BITSTREAM_ANNEXB, test, AVB_HARDWARE_PREFER);
  if (hardware.opened) {
    check_video_packets(hardware, false, test);
    decode_video(backend, hardware, nullptr, 0, test, "hardware", AVB_HARDWARE_PREFER);
  }

  // Without low latency a decoder may hold pictures back to reorder them;
  // they must still all come out, at the latest on drain.
  test.section("H.264 decode with reordering");
  decode_video(backend, annexb, nullptr, 0, test, "reorder", AVB_HARDWARE_DISABLED, 0);
  decode_video(backend, annexb, nullptr, 0, test, "reorder-hardware", AVB_HARDWARE_PREFER, 0);

  test.section("AAC round trip");
  audio_round_trip(backend, AVB_AUDIO_CODEC_AAC, test);

  test.section("Opus round trip");
  audio_round_trip(backend, AVB_AUDIO_CODEC_OPUS, test);

  return test.finish("stream round trip");
}
