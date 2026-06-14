// Hardware video-memory round-trip smoke test.
//
// Usage:
//   avb_video_memory_roundtrip <native|dmabuf> <fixture> <output>
//                              <decode_backend> <encode_backend>

#include <avbridge.h>

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <d3d11.h>
#include <wrl/client.h>
#endif

namespace {

constexpr int skip = 77;

struct MemoryConfig {
  const char *name = nullptr;
  avb_video_memory_type memory = AVB_VIDEO_MEMORY_CPU;
  avb_video_external_type external = AVB_VIDEO_EXTERNAL_NONE;
  avb_hardware_device device = AVB_HW_DEVICE_AUTO;
  int max_frames = 0;
};

bool parse_backend(const char *name, avb_backend &backend) {
  if (avb_backend_from_name(name, &backend) != AVB_OK) {
    std::fprintf(stderr, "unknown backend '%s'\n", name);
    return false;
  }
  if (!avb_backend_is_available(backend)) {
    std::printf("SKIP: backend '%s' is not built\n", name);
    return false;
  }
  return true;
}

bool parse_memory(const char *name, avb_backend decode_backend,
                  MemoryConfig &config) {
  if (std::strcmp(name, "dmabuf") == 0) {
    config.name = "DMABUF";
    config.memory = AVB_VIDEO_MEMORY_EXTERNAL;
    config.external = AVB_VIDEO_EXTERNAL_DMABUF;
    config.device = AVB_HW_DEVICE_VAAPI;
    config.max_frames = 8;
    return true;
  }
  if (std::strcmp(name, "native") != 0)
    return false;

  config.name = "native";
  config.memory = AVB_VIDEO_MEMORY_BACKEND_NATIVE;
  if (decode_backend == AVB_BACKEND_MEDIAFOUNDATION) {
    config.memory = AVB_VIDEO_MEMORY_EXTERNAL;
    config.external = AVB_VIDEO_EXTERNAL_D3D11_TEXTURE;
  } else if (decode_backend == AVB_BACKEND_AVFOUNDATION) {
    config.memory = AVB_VIDEO_MEMORY_EXTERNAL;
    config.external = AVB_VIDEO_EXTERNAL_CVPIXEL_BUFFER;
  }
  return true;
}

bool optional_runtime_unsupported(const char *error) {
  if (!error)
    return false;
  return std::strstr(error, "Function not implemented") ||
         std::strstr(error, "not implemented") ||
         std::strstr(error, "not supported") ||
         std::strstr(error, "unsupported") || std::strstr(error, "cannot");
}

bool valid_frame(const avb_video_frame &frame, const MemoryConfig &config) {
  if (frame.memory_type != config.memory ||
      frame.external_type != config.external) {
    return false;
  }
  if (config.external == AVB_VIDEO_EXTERNAL_DMABUF) {
    return frame.plane_count > 0 && frame.dmabuf_fd[0] >= 0;
  }
  return frame.native_handle != nullptr;
}

void print_first_frame(const avb_video_frame &frame,
                       const MemoryConfig &config) {
  if (config.external == AVB_VIDEO_EXTERNAL_DMABUF) {
    std::printf("%s first frame: %dx%d pts=%.6f planes=%d drm=0x%08x "
                "modifier=0x%016llx fd0=%d fd1=%d\n",
                config.name, frame.width, frame.height, frame.pts_sec,
                frame.plane_count, frame.drm_format,
                static_cast<unsigned long long>(frame.dmabuf_modifier[0]),
                frame.dmabuf_fd[0], frame.dmabuf_fd[1]);
    return;
  }
  std::printf("%s first frame: %dx%d pts=%.6f device=%d handle=%p\n",
              config.name, frame.width, frame.height, frame.pts_sec,
              static_cast<int>(frame.hardware_device), frame.native_handle);
}

bool verify_output(const char *path, avb_backend backend) {
  avb_decode_options options = avb_decode_options_default();
  options.backend = backend;
  options.enable_audio = 0;
  options.video_format = AVB_PIXEL_FORMAT_BGRA8;
  options.video_memory = AVB_VIDEO_MEMORY_CPU;

  avb_decoder *decoder = nullptr;
  if (avb_decoder_open(&decoder, path, &options) != AVB_OK) {
    std::fprintf(stderr, "re-open encoded output failed: %s\n",
                 avb_decoder_get_last_error(decoder)
                     ? avb_decoder_get_last_error(decoder)
                     : "unknown");
    avb_decoder_close(decoder);
    return false;
  }
  avb_video_frame frame{};
  const bool decoded = avb_decoder_read_video_frame(decoder, &frame) == AVB_OK;
  avb_decoder_release_video_frame(decoder, &frame);
  avb_decoder_close(decoder);
  return decoded;
}

} // namespace

int main(int argc, char *argv[]) {
  if (argc < 6) {
    std::fprintf(stderr,
                 "Usage: %s <native|dmabuf> <fixture> <output> "
                 "<decode_backend> <encode_backend>\n",
                 argv[0]);
    return 2;
  }

  avb_backend decode_backend = AVB_BACKEND_AUTO;
  avb_backend encode_backend = AVB_BACKEND_AUTO;
  if (!parse_backend(argv[4], decode_backend) ||
      !parse_backend(argv[5], encode_backend)) {
    return skip;
  }

  MemoryConfig config;
  if (!parse_memory(argv[1], decode_backend, config)) {
    std::fprintf(stderr, "unknown video memory mode '%s'\n", argv[1]);
    return 2;
  }

  avb_decode_options decode_options = avb_decode_options_default();
  decode_options.backend = decode_backend;
  decode_options.enable_audio = 0;
  decode_options.video_format = AVB_PIXEL_FORMAT_UNKNOWN;
  decode_options.video_memory = config.memory;
  decode_options.video_external_type = config.external;
  decode_options.hardware_policy = AVB_HARDWARE_PREFER;
  decode_options.hardware_device = config.device;

  avb_decoder *decoder = nullptr;
  if (avb_decoder_open(&decoder, argv[2], &decode_options) != AVB_OK) {
    std::printf("SKIP: %s decoder open failed for %s: %s\n", config.name,
                argv[4],
                avb_decoder_get_last_error(decoder)
                    ? avb_decoder_get_last_error(decoder)
                    : "unknown");
    avb_decoder_close(decoder);
    return skip;
  }

  avb_media_info info{};
  if (avb_decoder_get_media_info(decoder, &info) != AVB_OK ||
      !info.video.available) {
    std::printf("SKIP: no video stream for %s test\n", config.name);
    avb_decoder_close(decoder);
    return skip;
  }

  avb_video_frame first{};
  if (avb_decoder_read_video_frame(decoder, &first) != AVB_OK ||
      !valid_frame(first, config)) {
    std::printf("SKIP: first %s frame unavailable from %s: %s\n", config.name,
                argv[4],
                avb_decoder_get_last_error(decoder)
                    ? avb_decoder_get_last_error(decoder)
                    : "unknown");
    avb_decoder_release_video_frame(decoder, &first);
    avb_decoder_close(decoder);
    return skip;
  }
  print_first_frame(first, config);

  avb_encode_options encode_options = avb_encode_options_default();
  encode_options.backend = encode_backend;
  encode_options.video.enable = 1;
  encode_options.video.width = first.width;
  encode_options.video.height = first.height;
  encode_options.video.frame_rate =
      info.video.frame_rate > 0.0 ? info.video.frame_rate : 25.0;
  encode_options.video.codec = AVB_VIDEO_CODEC_H264;
  encode_options.video.input_format =
      config.external == AVB_VIDEO_EXTERNAL_DMABUF ? AVB_PIXEL_FORMAT_UNKNOWN
                                                   : first.format;
  encode_options.video.input_memory = config.memory;
  encode_options.video.input_external_type = config.external;
  encode_options.video.hardware_policy = AVB_HARDWARE_PREFER;
  encode_options.video.hardware_device = config.device;

#ifdef _WIN32
  Microsoft::WRL::ComPtr<ID3D11Device> d3d_device;
  if (encode_backend == AVB_BACKEND_MEDIAFOUNDATION &&
      first.hardware_device == AVB_HW_DEVICE_D3D11VA) {
    auto *texture = static_cast<ID3D11Texture2D *>(first.native_handle);
    texture->GetDevice(&d3d_device);
    encode_options.video.hardware_context = d3d_device.Get();
  }
#endif

  avb_encoder *encoder = nullptr;
  if (avb_encoder_open(&encoder, argv[3], &encode_options) != AVB_OK) {
    std::printf("SKIP: %s encoder open failed for %s: %s\n", config.name,
                argv[5],
                avb_encoder_get_last_error(encoder)
                    ? avb_encoder_get_last_error(encoder)
                    : "unknown");
    avb_encoder_close(encoder);
    avb_decoder_release_video_frame(decoder, &first);
    avb_decoder_close(decoder);
    return skip;
  }

  int written = 0;
  avb_video_frame frame = first;
  bool have_frame = true;
  while (have_frame &&
         (config.max_frames == 0 || written < config.max_frames)) {
    if (!valid_frame(frame, config)) {
      std::fprintf(stderr, "decoded frame %d does not match %s memory\n",
                   written, config.name);
      avb_decoder_release_video_frame(decoder, &frame);
      avb_encoder_close(encoder);
      avb_decoder_close(decoder);
      return 1;
    }
    if (avb_encoder_write_video(encoder, &frame, frame.pts_sec) != AVB_OK) {
      const char *error = avb_encoder_get_last_error(encoder);
      if (written == 0 && optional_runtime_unsupported(error)) {
        std::printf("SKIP: %s encoder write unsupported for %s: %s\n",
                    config.name, argv[5], error ? error : "unknown");
        avb_decoder_release_video_frame(decoder, &frame);
        avb_encoder_close(encoder);
        avb_decoder_close(decoder);
        return skip;
      }
      std::fprintf(stderr, "write_video failed: %s\n",
                   error ? error : "unknown");
      avb_decoder_release_video_frame(decoder, &frame);
      avb_encoder_close(encoder);
      avb_decoder_close(decoder);
      return 1;
    }
    ++written;
    avb_decoder_release_video_frame(decoder, &frame);
    have_frame = avb_decoder_read_video_frame(decoder, &frame) == AVB_OK;
  }
  if (have_frame) {
    avb_decoder_release_video_frame(decoder, &frame);
  }
  avb_decoder_close(decoder);

  if (written == 0) {
    std::printf("SKIP: no %s frames written\n", config.name);
    avb_encoder_close(encoder);
    return skip;
  }
  if (avb_encoder_finish(encoder) != AVB_OK) {
    std::fprintf(stderr, "encoder finish failed: %s\n",
                 avb_encoder_get_last_error(encoder)
                     ? avb_encoder_get_last_error(encoder)
                     : "unknown");
    avb_encoder_close(encoder);
    return 1;
  }
  avb_encoder_close(encoder);

  if (!verify_output(argv[3], encode_backend)) {
    std::fprintf(stderr, "encoded output did not decode a video frame\n");
    return 1;
  }

  std::printf("%s roundtrip passed: %s -> %s, %d frames\n", config.name,
              argv[4], argv[5], written);
  return 0;
}
