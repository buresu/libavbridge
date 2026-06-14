// AVFoundation-specific CVPixelBuffer and frame-lease contract.
//
// Usage: avb_platform_avfoundation <fixture.mp4>

#include "test.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurface.h>

#include <array>
#include <cstdio>

namespace {

using avb::test::Context;

bool has_external(const avb_decoder_capabilities &caps,
                  avb_video_external_type type) {
  return avb::test::contains(caps.video_external_types,
                             caps.video_external_type_count, type);
}

bool has_device(const avb_decoder_capabilities &caps,
                avb_hardware_device device) {
  return avb::test::contains(caps.hardware_devices, caps.hardware_device_count,
                             device);
}

} // namespace

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s <fixture.mp4>\n", argv[0]);
    return 2;
  }
  if (!avb_backend_is_available(AVB_BACKEND_AVFOUNDATION)) {
    std::printf("SKIP: AVFoundation backend is not built\n");
    return avb::test::skip;
  }

  Context test;
  test.section("AVFoundation video memory");

  avb_decoder_capabilities caps{};
  test.equal(avb_decoder_probe_runtime_capabilities(AVB_BACKEND_AVFOUNDATION,
                                                    argv[1], &caps),
             AVB_OK, "AVFoundation runtime capability probe runs");
  if (caps.result != AVB_OK) {
    std::printf("SKIP: AVFoundation runtime is unavailable\n");
    return avb::test::skip;
  }
  test.check(has_external(caps, AVB_VIDEO_EXTERNAL_CVPIXEL_BUFFER),
             "AVFoundation advertises CVPixelBuffer output");
  test.check(has_device(caps, AVB_HW_DEVICE_VIDEOTOOLBOX),
             "AVFoundation advertises VideoToolbox");

  avb_decode_options options = avb_decode_options_default();
  options.backend = AVB_BACKEND_AVFOUNDATION;
  options.enable_audio = 0;
  options.video_format = AVB_PIXEL_FORMAT_UNKNOWN;
  options.video_memory = AVB_VIDEO_MEMORY_EXTERNAL;
  options.video_external_type = AVB_VIDEO_EXTERNAL_CVPIXEL_BUFFER;
  options.hardware_policy = AVB_HARDWARE_PREFER;
  options.hardware_device = AVB_HW_DEVICE_VIDEOTOOLBOX;

  avb_decoder *decoder = nullptr;
  if (avb_decoder_open(&decoder, argv[1], &options) != AVB_OK) {
    std::printf("SKIP: AVFoundation external decoder did not open: %s\n",
                avb::test::decoder_error(decoder));
    avb_decoder_close(decoder);
    return avb::test::skip;
  }

  std::array<avb_video_frame, 3> frames{};
  int held = 0;
  for (; held < static_cast<int>(frames.size()); ++held) {
    if (avb_decoder_read_video_frame(decoder, &frames[held]) != AVB_OK)
      break;

    const avb_video_frame &frame = frames[held];
    test.equal(frame.memory_type, AVB_VIDEO_MEMORY_EXTERNAL,
               "AVFoundation frame uses external memory");
    test.equal(frame.external_type, AVB_VIDEO_EXTERNAL_CVPIXEL_BUFFER,
               "AVFoundation frame is a CVPixelBuffer");
    test.equal(frame.hardware_device, AVB_HW_DEVICE_VIDEOTOOLBOX,
               "CVPixelBuffer frame reports VideoToolbox");
    test.check(frame.native_handle != nullptr,
               "CVPixelBuffer frame has a native handle");
    test.check(frame.native_owner != nullptr,
               "CVPixelBuffer frame has a lifetime owner");
    test.equal(frame.plane_count, 0,
               "external CVPixelBuffer exposes no CPU planes");

    if (!frame.native_handle)
      continue;
    auto pixel_buffer = static_cast<CVPixelBufferRef>(frame.native_handle);
    test.equal(static_cast<int>(CVPixelBufferGetWidth(pixel_buffer)),
               frame.width, "CVPixelBuffer width matches frame metadata");
    test.equal(static_cast<int>(CVPixelBufferGetHeight(pixel_buffer)),
               frame.height, "CVPixelBuffer height matches frame metadata");
    test.check(CVPixelBufferGetIOSurface(pixel_buffer) != nullptr,
               "CVPixelBuffer is IOSurface-backed");
  }

  test.equal(held, 3, "three CVPixelBuffer frame leases can be held together");
  if (held == 3) {
    test.check(frames[0].native_handle != frames[1].native_handle &&
                   frames[1].native_handle != frames[2].native_handle,
               "held CVPixelBuffer frames have independent backing objects");
  }

  CVPixelBufferRef retained = nullptr;
  if (held > 0 && frames[0].native_handle) {
    retained = static_cast<CVPixelBufferRef>(frames[0].native_handle);
    CVPixelBufferRetain(retained);
  }
  for (int index = held - 1; index >= 0; --index) {
    avb_decoder_release_video_frame(decoder, &frames[index]);
    test.check(frames[index].native_handle == nullptr &&
                   frames[index].native_owner == nullptr,
               "releasing a CVPixelBuffer frame clears its lease");
  }
  if (retained) {
    test.check(CVPixelBufferGetWidth(retained) > 0,
               "caller-retained CVPixelBuffer survives avbridge release");
    CVPixelBufferRelease(retained);
  }

  double landed = -1.0;
  test.equal(avb_decoder_seek(decoder, 0.0, &landed), AVB_OK,
             "AVFoundation external decoder seeks after releasing held frames");
  avb_video_frame after_seek{};
  test.equal(avb_decoder_read_video_frame(decoder, &after_seek), AVB_OK,
             "AVFoundation returns a CVPixelBuffer after seek");
  avb_decoder_release_video_frame(decoder, &after_seek);
  avb_decoder_close(decoder);

  return test.finish("platform_avfoundation");
}
