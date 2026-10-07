// AVFoundation-specific CVPixelBuffer and frame-lease contract, and the
// hardware policy contract for frames copied out to CPU memory.
//
// Usage: avb_platform_avfoundation <fixture.mp4>

#include "cpu_decode.hpp"
#include "test.hpp"

#import <AVFoundation/AVFoundation.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurface.h>
#import <VideoToolbox/VideoToolbox.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using avb::test::Context;
using avb::test::CpuDecode;
using avb::test::decode_cpu;
using avb::test::mean_luma_difference;

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

// The first frame as VideoToolbox's software decoder produces it, decoded here
// without avbridge: the picture a frame from the hardware decoder is held to.
// Empty luma when there is no software decoder for the stream.
CpuDecode decode_software_reference(const char *path) {
  CpuDecode out;
  @autoreleasepool {
    AVAsset *asset =
        [AVAsset assetWithURL:[NSURL fileURLWithPath:@(path)]];
    __block AVAssetTrack *track = nil;
    dispatch_semaphore_t loaded = dispatch_semaphore_create(0);
    [asset loadTracksWithMediaType:AVMediaTypeVideo
                 completionHandler:^(NSArray<AVAssetTrack *> *tracks,
                                     NSError *) {
                   track = tracks.firstObject;
                   dispatch_semaphore_signal(loaded);
                 }];
    dispatch_semaphore_wait(loaded, DISPATCH_TIME_FOREVER);
    if (!track || track.formatDescriptions.count == 0)
      return out;

    // No output settings: the reader hands over the samples still compressed.
    AVAssetReader *reader = [AVAssetReader assetReaderWithAsset:asset
                                                          error:nil];
    AVAssetReaderTrackOutput *samples =
        [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track
                                                   outputSettings:nil];
    if (!reader || ![reader canAddOutput:samples])
      return out;
    [reader addOutput:samples];
    if (![reader startReading])
      return out;
    CMSampleBufferRef sample = [samples copyNextSampleBuffer];
    while (sample && CMSampleBufferGetNumSamples(sample) == 0) {
      CFRelease(sample);
      sample = [samples copyNextSampleBuffer];
    }
    if (!sample)
      return out;

    NSDictionary *software = @{
      (NSString *)
      kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder : @NO,
    };
    NSDictionary *nv12 = @{
      (NSString *)kCVPixelBufferPixelFormatTypeKey :
          @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
    };
    VTDecompressionSessionRef session = nullptr;
    const OSStatus created = VTDecompressionSessionCreate(
        kCFAllocatorDefault,
        (__bridge CMVideoFormatDescriptionRef)track.formatDescriptions[0],
        (__bridge CFDictionaryRef)software, (__bridge CFDictionaryRef)nv12,
        nullptr, &session);
    if (created == noErr && session) {
      CpuDecode *decoded = &out;
      VTDecompressionSessionDecodeFrameWithOutputHandler(
          session, sample, 0, nullptr,
          ^(OSStatus status, VTDecodeInfoFlags, CVImageBufferRef image, CMTime,
            CMTime) {
            if (status != noErr || !image || !decoded->luma.empty())
              return;
            CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
            decoded->width = static_cast<int>(CVPixelBufferGetWidth(image));
            decoded->height = static_cast<int>(CVPixelBufferGetHeight(image));
            const auto *base = static_cast<const unsigned char *>(
                CVPixelBufferGetBaseAddressOfPlane(image, 0));
            const size_t stride = CVPixelBufferGetBytesPerRowOfPlane(image, 0);
            decoded->luma.resize(static_cast<size_t>(decoded->width) *
                                 decoded->height);
            for (int y = 0; y < decoded->height; ++y)
              std::memcpy(decoded->luma.data() +
                              static_cast<size_t>(y) * decoded->width,
                          base + static_cast<size_t>(y) * stride,
                          static_cast<size_t>(decoded->width));
            CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
          });
      VTDecompressionSessionWaitForAsynchronousFrames(session);
      VTDecompressionSessionInvalidate(session);
      CFRelease(session);
    }
    CFRelease(sample);
    [reader cancelReading];
  }
  return out;
}

// CPU frames under each hardware policy. The decoder is VideoToolbox's choice
// and AVAssetReader has no switch for it, so here the policy does not decide
// which one decodes the way it does on the other backends: REQUIRE checks the
// choice, PREFER and DISABLED take it, and a frame says what it was each time.
void check_cpu_frames(Context &test, const char *path) {
  const CpuDecode disabled =
      decode_cpu(path, AVB_BACKEND_AVFOUNDATION, AVB_HARDWARE_DISABLED);
  test.equal(disabled.open_result, AVB_OK, "DISABLED opens");
  test.check(disabled.cpu_memory, "DISABLED yields NV12 CPU frames");
  test.check(disabled.frames > 0, "DISABLED yields frames");

  const CpuDecode required =
      decode_cpu(path, AVB_BACKEND_AVFOUNDATION, AVB_HARDWARE_REQUIRE);
  const bool hardware = required.open_result == AVB_OK;
  const CpuDecode preferred =
      decode_cpu(path, AVB_BACKEND_AVFOUNDATION, AVB_HARDWARE_PREFER);
  test.equal(preferred.open_result, AVB_OK,
             "PREFER opens with or without a hardware decoder");
  test.check(preferred.cpu_memory, "PREFER yields NV12 CPU frames");
  test.equal(preferred.frames, disabled.frames,
             "PREFER decodes as many frames as DISABLED");
  test.check(preferred.survived_audio_read,
             "a PREFER frame stays intact until it is released");
  test.equal(preferred.color_range, AVB_COLOR_RANGE_LIMITED,
             "NV12 frames are video range and say so");
  test.equal(disabled.device, preferred.device,
             "a frame names what decoded it whatever the policy");

  avb_decode_options elsewhere = avb_decode_options_default();
  elsewhere.backend = AVB_BACKEND_AVFOUNDATION;
  elsewhere.video_format = AVB_PIXEL_FORMAT_NV12;
  elsewhere.hardware_policy = AVB_HARDWARE_REQUIRE;
  elsewhere.hardware_device = AVB_HW_DEVICE_VAAPI;
  avb_decoder *decoder = nullptr;
  test.check(avb_decoder_open(&decoder, path, &elsewhere) != AVB_OK,
             "REQUIRE does not open for another platform's device");
  avb_decoder_close(decoder);

  if (!hardware) {
    test.equal(preferred.device, AVB_HW_DEVICE_AUTO,
               "PREFER falls back to software without a hardware decoder");
    std::printf("SKIP: VideoToolbox has no hardware decoder for this stream\n");
    return;
  }

  test.check(required.cpu_memory, "REQUIRE yields NV12 CPU frames");
  test.equal(required.device, AVB_HW_DEVICE_VIDEOTOOLBOX,
             "a frame copied out of the hardware decoder reports VideoToolbox");
  test.equal(preferred.device, AVB_HW_DEVICE_VIDEOTOOLBOX,
             "PREFER takes the hardware decoder when there is one");
  test.equal(required.frames, disabled.frames,
             "REQUIRE decodes as many frames as DISABLED");
  test.check(required.survived_audio_read,
             "a hardware frame stays intact until it is released");

  const CpuDecode software = decode_software_reference(path);
  if (software.luma.empty()) {
    std::printf("SKIP: VideoToolbox has no software decoder for this stream\n");
    return;
  }
  test.equal(required.width, software.width,
             "hardware and software decode the same width");
  test.equal(required.height, software.height,
             "hardware and software decode the same height");
  test.near(mean_luma_difference(required, software), 0.0, 1.0,
            "a frame copied out of the hardware decoder matches the software "
            "decode");
  std::printf("AVFoundation CPU frame contract passed (VideoToolbox)\n");
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

  test.section("AVFoundation hardware policy for CPU frames");
  check_cpu_frames(test, argv[1]);

  test.section("AVFoundation CVPixelBuffer frames");
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
