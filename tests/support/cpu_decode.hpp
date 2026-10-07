#pragma once

// Decoding a file into CPU frames under one hardware policy, for the platform
// tests that hold the policies to each other.

#include <avbridge.h>

#include <cmath>
#include <cstring>
#include <vector>

namespace avb::test {

// What a decode under one hardware policy produced: the first frame's luma,
// tightly packed, what decoded it, and how many frames followed.
struct CpuDecode {
  avb_result open_result = AVB_ERROR_UNKNOWN;
  avb_hardware_device device = AVB_HW_DEVICE_AUTO;
  avb_color_range color_range = AVB_COLOR_RANGE_UNKNOWN;
  avb_color_matrix color_matrix = AVB_COLOR_MATRIX_UNKNOWN;
  int width = 0;
  int height = 0;
  int frames = 0;
  bool cpu_memory = true;
  bool survived_audio_read = true;
  std::vector<unsigned char> luma;
};

inline std::vector<unsigned char> tight_luma(const avb_video_frame &frame) {
  std::vector<unsigned char> luma(static_cast<size_t>(frame.width) *
                                  frame.height);
  for (int y = 0; y < frame.height; ++y)
    std::memcpy(luma.data() + static_cast<size_t>(y) * frame.width,
                frame.plane_data[0] +
                    static_cast<size_t>(y) * frame.plane_stride[0],
                static_cast<size_t>(frame.width));
  return luma;
}

inline CpuDecode decode_cpu(const char *path, avb_backend backend,
                            avb_hardware_policy policy) {
  CpuDecode out;
  avb_decode_options options = avb_decode_options_default();
  options.backend = backend;
  options.video_format = AVB_PIXEL_FORMAT_NV12;
  options.video_memory = AVB_VIDEO_MEMORY_CPU;
  options.hardware_policy = policy;

  avb_decoder *decoder = nullptr;
  out.open_result = avb_decoder_open(&decoder, path, &options);
  if (out.open_result != AVB_OK) {
    avb_decoder_close(decoder);
    return out;
  }

  std::vector<float> audio(4096);
  avb_video_frame frame{};
  while (avb_decoder_read_video_frame(decoder, &frame) == AVB_OK) {
    out.cpu_memory = out.cpu_memory &&
                     frame.memory_type == AVB_VIDEO_MEMORY_CPU &&
                     frame.format == AVB_PIXEL_FORMAT_NV12 &&
                     frame.plane_count == 2 && frame.plane_data[0] &&
                     frame.plane_data[1];
    if (!out.cpu_memory) {
      avb_decoder_release_video_frame(decoder, &frame);
      break;
    }
    if (out.frames == 0) {
      out.device = frame.hardware_device;
      out.color_range = frame.color_range;
      out.color_matrix = frame.color_matrix;
      out.width = frame.width;
      out.height = frame.height;
      out.luma = tight_luma(frame);
      // The frame is the caller's until it is released, whatever else the
      // decoder is asked for in between.
      avb_decoder_read_audio_f32(decoder, audio.data(), 1024, nullptr);
      out.survived_audio_read = tight_luma(frame) == out.luma;
    }
    avb_decoder_release_video_frame(decoder, &frame);
    ++out.frames;
    avb_decoder_read_audio_f32(decoder, audio.data(), 1024, nullptr);
  }
  avb_decoder_close(decoder);
  return out;
}

inline double mean_luma_difference(const CpuDecode &a, const CpuDecode &b) {
  if (a.luma.size() != b.luma.size() || a.luma.empty())
    return 255.0;
  double sum = 0.0;
  for (size_t i = 0; i < a.luma.size(); ++i)
    sum += std::fabs(static_cast<double>(a.luma[i]) - b.luma[i]);
  return sum / static_cast<double>(a.luma.size());
}

} // namespace avb::test
