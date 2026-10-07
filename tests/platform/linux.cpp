// Linux-specific DMABUF descriptor and lifetime contract, and the hardware
// policy contract for frames read back into CPU memory.
//
// Usage: avb_platform_linux <fixture.mp4>

#include "test.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

using avb::test::Context;

bool has_external(const avb_decoder_capabilities &caps,
                  avb_video_external_type type) {
  return avb::test::contains(caps.video_external_types,
                             caps.video_external_type_count, type);
}

bool has_memory(const avb_decoder_capabilities &caps,
                avb_video_memory_type memory) {
  return avb::test::contains(caps.video_memory, caps.video_memory_count,
                             memory);
}

bool has_device(const avb_decoder_capabilities &caps,
                avb_hardware_device device) {
  return avb::test::contains(caps.hardware_devices, caps.hardware_device_count,
                             device);
}

bool check_backend(Context &test, const char *path, avb_backend backend) {
  if (!avb_backend_is_available(backend))
    return false;

  avb_decoder_capabilities caps{};
  test.equal(avb_decoder_probe_runtime_capabilities(backend, path, &caps),
             AVB_OK, "Linux decoder runtime capability probe runs");
  if (caps.result != AVB_OK) {
    std::printf("SKIP: %s runtime is unavailable\n", avb_backend_name(backend));
    return false;
  }

  const bool advertises_dmabuf = has_external(caps, AVB_VIDEO_EXTERNAL_DMABUF);
  test.check(!advertises_dmabuf ||
                 (has_memory(caps, AVB_VIDEO_MEMORY_EXTERNAL) &&
                  has_device(caps, AVB_HW_DEVICE_VAAPI)),
             "DMABUF capability includes external memory and VAAPI");
  if (!advertises_dmabuf) {
    std::printf("SKIP: %s runtime does not advertise DMABUF\n",
                avb_backend_name(backend));
    return false;
  }

  avb_decode_options options = avb_decode_options_default();
  options.backend = backend;
  options.enable_audio = 0;
  options.video_format = AVB_PIXEL_FORMAT_UNKNOWN;
  options.video_memory = AVB_VIDEO_MEMORY_EXTERNAL;
  options.video_external_type = AVB_VIDEO_EXTERNAL_DMABUF;
  options.hardware_policy = AVB_HARDWARE_PREFER;
  options.hardware_device = AVB_HW_DEVICE_VAAPI;

  avb_decoder *decoder = nullptr;
  if (avb_decoder_open(&decoder, path, &options) != AVB_OK) {
    std::printf("SKIP: %s DMABUF decoder did not open: %s\n",
                avb_backend_name(backend), avb::test::decoder_error(decoder));
    avb_decoder_close(decoder);
    return false;
  }

  avb_video_frame frame{};
  if (avb_decoder_read_video_frame(decoder, &frame) != AVB_OK) {
    std::printf("SKIP: %s produced no DMABUF frame: %s\n",
                avb_backend_name(backend), avb::test::decoder_error(decoder));
    avb_decoder_close(decoder);
    return false;
  }

  test.equal(frame.memory_type, AVB_VIDEO_MEMORY_EXTERNAL,
             "Linux hardware frame uses external memory");
  test.equal(frame.external_type, AVB_VIDEO_EXTERNAL_DMABUF,
             "Linux hardware frame is DMABUF");
  test.equal(frame.hardware_device, AVB_HW_DEVICE_VAAPI,
             "Linux DMABUF frame reports VAAPI");
  test.check(frame.native_handle != nullptr, "DMABUF frame has native handle");
  test.check(frame.native_owner != nullptr,
             "DMABUF frame has a lifetime owner");
  test.check(frame.drm_format != 0, "DMABUF frame has a DRM format");
  test.equal(frame.native_handle_id, static_cast<uint64_t>(frame.drm_format),
             "DMABUF native id matches DRM format");
  test.check(frame.plane_count > 0 && frame.plane_count <= AVB_MAX_PLANES,
             "DMABUF plane count is valid");

  std::set<int> source_fds;
  std::vector<int> duplicated_fds;
  for (int plane = 0; plane < frame.plane_count; ++plane) {
    test.check(frame.dmabuf_fd[plane] >= 0, "DMABUF plane has an fd");
    test.check(frame.plane_stride[plane] > 0,
               "DMABUF plane has a positive stride");
    test.check(frame.plane_offset[plane] >= 0,
               "DMABUF plane has a non-negative offset");
    if (frame.dmabuf_fd[plane] < 0 ||
        !source_fds.insert(frame.dmabuf_fd[plane]).second) {
      continue;
    }
    const int duplicate = dup(frame.dmabuf_fd[plane]);
    test.check(duplicate >= 0,
               "DMABUF fd can be duplicated while the frame lease is held");
    if (duplicate >= 0)
      duplicated_fds.push_back(duplicate);
  }

  avb_decoder_release_video_frame(decoder, &frame);
  test.check(frame.native_handle == nullptr && frame.native_owner == nullptr,
             "releasing a DMABUF frame clears its lease");
  avb_decoder_close(decoder);

  for (int fd : duplicated_fds) {
    test.check(fcntl(fd, F_GETFD) != -1,
               "duplicated DMABUF fd remains valid after frame release");
    close(fd);
  }

  std::printf("%s Linux DMABUF contract passed\n", avb_backend_name(backend));
  return true;
}

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

std::vector<unsigned char> tight_luma(const avb_video_frame &frame) {
  std::vector<unsigned char> luma(static_cast<size_t>(frame.width) *
                                  frame.height);
  for (int y = 0; y < frame.height; ++y)
    std::memcpy(luma.data() + static_cast<size_t>(y) * frame.width,
                frame.plane_data[0] +
                    static_cast<size_t>(y) * frame.plane_stride[0],
                static_cast<size_t>(frame.width));
  return luma;
}

CpuDecode decode_cpu(const char *path, avb_backend backend,
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

double mean_luma_difference(const CpuDecode &a, const CpuDecode &b) {
  if (a.luma.size() != b.luma.size() || a.luma.empty())
    return 255.0;
  double sum = 0.0;
  for (size_t i = 0; i < a.luma.size(); ++i)
    sum += std::fabs(static_cast<double>(a.luma[i]) - b.luma[i]);
  return sum / static_cast<double>(a.luma.size());
}

// CPU frames come from either kind of decoder, and the policy decides which:
// DISABLED is software, REQUIRE is hardware or nothing, PREFER is hardware
// where there is some and always opens. All three show the same picture.
bool check_cpu_readback(Context &test, const char *path, avb_backend backend) {
  if (!avb_backend_is_available(backend))
    return false;
  const char *name = avb_backend_name(backend);

  const CpuDecode software = decode_cpu(path, backend, AVB_HARDWARE_DISABLED);
  if (software.open_result != AVB_OK) {
    std::printf("SKIP: %s runtime is unavailable\n", name);
    return false;
  }
  test.check(software.cpu_memory, "DISABLED yields NV12 CPU frames");
  test.equal(software.device, AVB_HW_DEVICE_AUTO,
             "DISABLED decodes in software");
  test.check(software.frames > 0, "software decode yields frames");
  test.check(software.survived_audio_read,
             "a software frame stays intact until it is released");

  const CpuDecode required = decode_cpu(path, backend, AVB_HARDWARE_REQUIRE);
  const bool hardware = required.open_result == AVB_OK;
  const CpuDecode preferred = decode_cpu(path, backend, AVB_HARDWARE_PREFER);
  test.equal(preferred.open_result, AVB_OK,
             "PREFER opens with or without a hardware decoder");
  test.check(preferred.cpu_memory, "PREFER yields NV12 CPU frames");
  test.equal(preferred.frames, software.frames,
             "PREFER decodes as many frames as software");
  test.near(mean_luma_difference(preferred, software), 0.0, 1.0,
            "PREFER shows the picture software decodes");
  test.check(preferred.survived_audio_read,
             "a PREFER frame stays intact until it is released");

  if (!hardware) {
    test.equal(preferred.device, AVB_HW_DEVICE_AUTO,
               "PREFER falls back to software without a hardware decoder");
    std::printf("SKIP: %s has no hardware decoder for this stream\n", name);
    return false;
  }

  test.check(required.cpu_memory, "REQUIRE yields NV12 CPU frames");
  test.check(required.device != AVB_HW_DEVICE_AUTO,
             "a frame read back from hardware names what decoded it");
  test.equal(required.frames, software.frames,
             "hardware decodes as many frames as software");
  test.near(mean_luma_difference(required, software), 0.0, 1.0,
            "a frame read back from hardware matches the software decode");
  test.check(required.survived_audio_read,
             "a hardware frame stays intact until it is released");
  test.equal(preferred.device, required.device,
             "PREFER takes the hardware decoder when there is one");
  // Same pixels read with a different range are a different picture. The
  // software decoder parses the stream; the hardware one must agree with it.
  test.equal(required.color_range, software.color_range,
             "hardware decode reports the color range software reads");
  test.equal(required.color_matrix, software.color_matrix,
             "hardware decode reports the color matrix software reads");
  std::printf("%s Linux CPU readback contract passed (device %d)\n", name,
              static_cast<int>(required.device));
  return true;
}

} // namespace

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s <fixture.mp4>\n", argv[0]);
    return 2;
  }

  Context test;
  test.section("Linux video memory");
  bool exercised = false;
  exercised |= check_backend(test, argv[1], AVB_BACKEND_GSTREAMER);
  exercised |= check_backend(test, argv[1], AVB_BACKEND_FFMPEG);

  test.section("Linux hardware policy for CPU frames");
  exercised |= check_cpu_readback(test, argv[1], AVB_BACKEND_GSTREAMER);
  exercised |= check_cpu_readback(test, argv[1], AVB_BACKEND_FFMPEG);

  const int result = test.finish("platform_linux");
  if (result != 0)
    return result;
  return exercised ? 0 : avb::test::skip;
}
