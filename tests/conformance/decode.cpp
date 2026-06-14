// Backend-independent decode contract exercised against one generated fixture.
//
// Usage: avb_conformance <fixture.mp4> [backend]

#include "test.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using avb::test::Context;

struct FormatCase {
  avb_pixel_format format;
  const char *name;
  int planes;
};

bool open_decoder(const char *path, avb_backend backend, bool audio, bool video,
                  avb_pixel_format format, avb_decoder **out_decoder,
                  avb_result *out_result = nullptr) {
  avb_decode_options options = avb_decode_options_default();
  options.backend = backend;
  options.enable_audio = audio ? 1 : 0;
  options.enable_video = video ? 1 : 0;
  options.video_format = format;
  const avb_result result = avb_decoder_open(out_decoder, path, &options);
  if (out_result)
    *out_result = result;
  return result == AVB_OK;
}

void check_probe(Context &test, const char *path, avb_backend backend,
                 const avb_media_info &info) {
  test.section("probe");

  avb_decode_options options = avb_decode_options_default();
  options.backend = backend;
  avb_media_probe probe{};
  test.equal(avb_probe_media(path, &options, &probe), AVB_OK, "probe succeeds");
  test.equal(probe.result, AVB_OK, "probe result is OK");
  test.string(probe.backend_name, info.backend_name,
              "probe backend matches decoder");
  test.near(probe.duration_sec, info.duration_sec, 0.01,
            "probe duration matches decoder");
  test.equal(probe.audio.available, info.audio.available,
             "probe audio availability matches");
  test.equal(probe.video.available, info.video.available,
             "probe video availability matches");
  test.string(probe.audio.codec_name, info.audio.codec_name,
              "probe audio codec matches");
  test.string(probe.video.codec_name, info.video.codec_name,
              "probe video codec matches");
}

void check_audio(Context &test, const char *path, avb_backend backend,
                 const avb_media_info &info) {
  test.section("audio decode");

  avb_decoder *decoder = nullptr;
  const bool opened = open_decoder(path, backend, true, false,
                                   AVB_PIXEL_FORMAT_UNKNOWN, &decoder);
  test.check(opened, "audio-only decoder opens");
  if (!opened) {
    std::fprintf(stderr, "audio-only open failed: %s\n",
                 avb::test::decoder_error(decoder));
    avb_decoder_close(decoder);
    return;
  }

  constexpr int block_size = 4096;
  std::vector<float> samples(
      block_size * (info.audio.channels > 0 ? info.audio.channels : 1));
  double sum_squares = 0.0;
  long long sample_count = 0;
  long long frame_count = 0;
  double previous_pts = -1.0;

  for (;;) {
    double pts = -1.0;
    const int frames =
        avb_decoder_read_audio_f32(decoder, samples.data(), block_size, &pts);
    if (frames <= 0)
      break;
    test.check(pts < 0.0 || previous_pts < 0.0 || pts >= previous_pts,
               "audio timestamps are monotonic when reported");
    previous_pts = pts;
    for (int i = 0; i < frames * info.audio.channels; ++i) {
      sum_squares += static_cast<double>(samples[i]) * samples[i];
    }
    sample_count += static_cast<long long>(frames) * info.audio.channels;
    frame_count += frames;
  }

  test.check(avb_decoder_audio_at_eof(decoder) == 1,
             "audio EOF is reported after draining");
  test.near(static_cast<double>(frame_count) / info.audio.sample_rate, 3.0, 0.2,
            "decoded audio duration is about 3 seconds");
  const double rms =
      sample_count > 0 ? std::sqrt(sum_squares / sample_count) : 0.0;
  test.check(rms > 0.001, "decoded audio is non-silent");

  double landed = -1.0;
  test.equal(avb_decoder_seek(decoder, -10.0, &landed), AVB_OK,
             "negative audio seek succeeds");
  test.near(landed, 0.0, 0.0, "negative seek clamps to zero");
  test.check(avb_decoder_audio_at_eof(decoder) == 0, "seek clears audio EOF");

  avb_decoder_close(decoder);
}

void check_audio_conversion(Context &test, const char *path,
                            avb_backend backend) {
  test.section("audio conversion");

  avb_decode_options options = avb_decode_options_default();
  options.backend = backend;
  options.enable_video = 0;
  options.audio_sample_rate = 22050;
  options.audio_channels = 2;

  avb_decoder *decoder = nullptr;
  const avb_result open_result = avb_decoder_open(&decoder, path, &options);
  test.equal(open_result, AVB_OK, "audio conversion decoder opens");
  if (open_result != AVB_OK) {
    std::fprintf(stderr, "audio conversion open failed: %s\n",
                 avb::test::decoder_error(decoder));
    avb_decoder_close(decoder);
    return;
  }

  avb_media_info info{};
  test.equal(avb_decoder_get_media_info(decoder, &info), AVB_OK,
             "converted audio info is available");
  test.equal(info.audio.sample_rate, 22050, "audio is resampled to 22050 Hz");
  test.equal(info.audio.channels, 2, "audio is remixed to stereo");

  std::vector<float> samples(4096 * 2);
  long long frames = 0;
  for (;;) {
    const int got =
        avb_decoder_read_audio_f32(decoder, samples.data(), 4096, nullptr);
    if (got <= 0)
      break;
    frames += got;
  }
  test.near(static_cast<double>(frames) / 22050.0, 3.0, 0.2,
            "converted audio duration is preserved");
  avb_decoder_close(decoder);
}

void check_cpu_frame_layout(Context &test, const avb_video_frame &frame,
                            const FormatCase &format) {
  test.equal(frame.format, format.format, "requested format is returned");
  test.equal(frame.memory_type, AVB_VIDEO_MEMORY_CPU, "frame is CPU memory");
  test.equal(frame.external_type, AVB_VIDEO_EXTERNAL_NONE,
             "CPU frame has no external type");
  test.equal(frame.plane_count, format.planes, "plane count is correct");
  test.check(frame.data == frame.plane_data[0], "data aliases plane zero");
  test.equal(frame.stride, frame.plane_stride[0],
             "stride aliases plane zero stride");
  test.check(frame.data_size > 0, "frame has a positive data size");

  for (int plane = 0; plane < frame.plane_count; ++plane) {
    test.check(frame.plane_data[plane] != nullptr,
               "every frame plane has data");
    test.check(frame.plane_stride[plane] > 0,
               "every frame plane has a positive stride");
  }

  long long expected_size = 0;
  switch (format.format) {
  case AVB_PIXEL_FORMAT_RGBA8:
  case AVB_PIXEL_FORMAT_BGRA8:
    expected_size =
        static_cast<long long>(frame.plane_stride[0]) * frame.height;
    break;
  case AVB_PIXEL_FORMAT_NV12:
    expected_size =
        static_cast<long long>(frame.plane_stride[0]) * frame.height +
        static_cast<long long>(frame.plane_stride[1]) *
            ((frame.height + 1) / 2);
    break;
  case AVB_PIXEL_FORMAT_I420:
    expected_size =
        static_cast<long long>(frame.plane_stride[0]) * frame.height +
        static_cast<long long>(frame.plane_stride[1]) *
            ((frame.height + 1) / 2) +
        static_cast<long long>(frame.plane_stride[2]) *
            ((frame.height + 1) / 2);
    break;
  default:
    break;
  }
  if (expected_size > 0) {
    test.equal(static_cast<long long>(frame.data_size), expected_size,
               "frame data size covers all planes");
  }
}

void check_video(Context &test, const char *path, avb_backend backend) {
  test.section("video decode");

  constexpr FormatCase formats[] = {
      {AVB_PIXEL_FORMAT_BGRA8, "BGRA8", 1},
      {AVB_PIXEL_FORMAT_RGBA8, "RGBA8", 1},
      {AVB_PIXEL_FORMAT_NV12, "NV12", 2},
      {AVB_PIXEL_FORMAT_I420, "I420", 3},
  };

  for (const FormatCase &format : formats) {
    avb_decoder *decoder = nullptr;
    avb_result open_result = AVB_OK;
    if (!open_decoder(path, backend, false, true, format.format, &decoder,
                      &open_result)) {
      if (format.format == AVB_PIXEL_FORMAT_BGRA8 ||
          format.format == AVB_PIXEL_FORMAT_RGBA8) {
        std::fprintf(stderr, "%s open failed: %s\n", format.name,
                     avb::test::decoder_error(decoder));
        test.check(false, "mandatory RGB format opens");
      } else {
        std::printf("SKIP: %s output is not supported by this backend\n",
                    format.name);
      }
      avb_decoder_close(decoder);
      continue;
    }

    int frame_count = 0;
    double previous_pts = -1.0;
    for (;;) {
      avb_video_frame frame{};
      const avb_result read = avb_decoder_read_video_frame(decoder, &frame);
      if (read == AVB_ERROR_EOF)
        break;
      if (read != AVB_OK) {
        std::fprintf(stderr, "%s decode failed: %s\n", format.name,
                     avb::test::decoder_error(decoder));
        test.check(false, "video read returns OK or EOF");
        break;
      }
      test.check(previous_pts < 0.0 || frame.pts_sec >= previous_pts,
                 "video timestamps are monotonic");
      previous_pts = frame.pts_sec;
      if (frame_count == 0) {
        test.equal(frame.width, 320, "frame width is 320");
        test.equal(frame.height, 240, "frame height is 240");
        check_cpu_frame_layout(test, frame, format);
      }
      avb_decoder_release_video_frame(decoder, &frame);
      ++frame_count;
    }
    test.near(frame_count, 75.0, 3.0, "decoded video frame count is about 75");
    avb_decoder_close(decoder);
  }
}

void check_seek(Context &test, const char *path, avb_backend backend,
                double duration) {
  test.section("seek");

  avb_decoder *decoder = nullptr;
  const bool opened = open_decoder(path, backend, false, true,
                                   AVB_PIXEL_FORMAT_BGRA8, &decoder);
  test.check(opened, "video seek decoder opens");
  if (!opened) {
    std::fprintf(stderr, "video seek open failed: %s\n",
                 avb::test::decoder_error(decoder));
    avb_decoder_close(decoder);
    return;
  }

  double landed = -1.0;
  test.equal(avb_decoder_seek(decoder, 1.5, &landed), AVB_OK,
             "seek to middle succeeds");
  test.near(landed, 1.5, 0.001, "middle seek reports requested landing");
  avb_video_frame frame{};
  const avb_result read = avb_decoder_read_video_frame(decoder, &frame);
  test.equal(read, AVB_OK, "frame decodes after seek");
  if (read == AVB_OK) {
    test.near(frame.pts_sec, 1.5, 0.25,
              "first frame after seek is near target");
    avb_decoder_release_video_frame(decoder, &frame);
  }

  test.equal(avb_decoder_seek(decoder, 999.0, &landed), AVB_OK,
             "past-end seek succeeds");
  test.near(landed, duration, 0.01, "past-end seek clamps to duration");
  avb_decoder_close(decoder);
}

} // namespace

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s <fixture.mp4> [backend]\n", argv[0]);
    return 2;
  }

  avb_backend backend = AVB_BACKEND_AUTO;
  if (argc >= 3 && !avb::test::parse_backend(argv[2], backend)) {
    std::fprintf(stderr, "unknown backend '%s'\n", argv[2]);
    return 2;
  }
  if (!avb::test::backend_is_built(backend)) {
    std::printf("SKIP: backend '%s' is not built\n", avb_backend_name(backend));
    return avb::test::skip;
  }

  avb_decoder *decoder = nullptr;
  avb_result open_result = AVB_OK;
  if (!open_decoder(argv[1], backend, true, true, AVB_PIXEL_FORMAT_BGRA8,
                    &decoder, &open_result)) {
    if (open_result == AVB_ERROR_BACKEND_NOT_AVAILABLE) {
      std::printf("SKIP: backend runtime is unavailable: %s\n",
                  avb::test::decoder_error(decoder));
      avb_decoder_close(decoder);
      return avb::test::skip;
    }
    std::fprintf(stderr, "fixture open failed: %s\n",
                 avb::test::decoder_error(decoder));
    avb_decoder_close(decoder);
    return 1;
  }

  Context test;
  test.section("media info");
  avb_media_info info{};
  test.equal(avb_decoder_get_media_info(decoder, &info), AVB_OK,
             "media info succeeds");
  test.check(info.backend_name != nullptr, "backend name is reported");
  test.near(info.duration_sec, 3.0, 0.2, "duration is about 3 seconds");
  test.equal(info.audio.available, 1, "audio stream is available");
  test.equal(info.audio.track_count, 1, "one audio track is reported");
  test.equal(info.audio.sample_rate, 44100, "audio rate is 44100 Hz");
  test.equal(info.audio.channels, 1, "audio is mono");
  test.string(info.audio.codec_name, "aac", "source audio codec is AAC");
  test.equal(info.video.available, 1, "video stream is available");
  test.equal(info.video.width, 320, "video width is 320");
  test.equal(info.video.height, 240, "video height is 240");
  test.near(info.video.frame_rate, 25.0, 1.0, "video rate is about 25 fps");
  test.string(info.video.codec_name, "h264", "source video codec is H.264");

  check_probe(test, argv[1], backend, info);
  avb_decoder_close(decoder);

  check_audio(test, argv[1], backend, info);
  check_audio_conversion(test, argv[1], backend);
  check_video(test, argv[1], backend);
  check_seek(test, argv[1], backend, info.duration_sec);
  return test.finish("conformance");
}
