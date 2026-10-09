// FFmpeg resampler lifecycle: exact output counts, delay-adjusted timestamps,
// and deterministic restart with pending samples or after EOF.
#include "test.hpp"

#include <algorithm>
#include <vector>

namespace {

void check_resampling(avb::test::Context &test, const char *path,
                      avb_backend backend, int rate, bool report_pts) {
  avb_decode_options options = avb_decode_options_default();
  options.backend = backend;
  options.enable_video = 0;
  options.audio_sample_rate = rate;
  avb_decoder *decoder = nullptr;
  const avb_result opened = avb_decoder_open(&decoder, path, &options);
  test.equal(opened, AVB_OK, "PCM resampling decoder opens");
  if (opened != AVB_OK) {
    std::fprintf(stderr, "%s\n", avb::test::decoder_error(decoder));
    avb_decoder_close(decoder);
    return;
  }

  std::vector<float> first(257);
  double pts = -1.0;
  test.equal(avb_decoder_read_audio_f32(decoder, first.data(), 257,
                                       report_pts ? &pts : nullptr),
             257, "initial samples decode");
  if (report_pts) test.near(pts, 0.0, 1.0 / rate, "initial PTS is zero");

  // Drain once without seeking, once after seeking with buffered samples,
  // and once after seeking back from EOF.
  for (int pass = 0; pass < 3; ++pass) {
    if (pass > 0) {
      test.equal(avb_decoder_seek(decoder, 0.0, nullptr), AVB_OK,
                 "resampling seek to zero succeeds");
      if (pass == 1) {
        std::vector<float> partial(1024);
        test.equal(avb_decoder_read_audio_f32(decoder, partial.data(), 1024, nullptr),
                   1024, "audio is read before a seek with buffered samples");
        test.equal(avb_decoder_seek(decoder, 0.0, nullptr), AVB_OK,
                   "seek with buffered samples succeeds");
      }
    }
    std::vector<float> block(257);
    int total = pass == 0 ? 257 : 0;
    double max_difference = 0.0;
    double max_pts_error = 0.0;
    for (int reads = 0; reads < rate; ++reads) {
      const int got = avb_decoder_read_audio_f32(
          decoder, block.data(), 257, report_pts ? &pts : nullptr);
      if (got <= 0) break;
      if (total == 0) {
        for (int i = 0; i < got; ++i)
          max_difference = std::max(max_difference,
              std::fabs(static_cast<double>(block[i]) - first[i]));
      }
      if (report_pts) {
        test.check(pts >= 0.0, "PCM audio reports a timestamp");
        max_pts_error = std::max(max_pts_error,
            std::fabs(pts - static_cast<double>(total) / rate));
      }
      total += got;
    }
    test.near(max_difference, 0.0, 1e-6,
               "seek clears all previous resampler history");
    test.near(total, rate, 1.0, "one second of PCM retains every resampled frame");
    if (report_pts)
      test.near(max_pts_error, 0.0, 2.0 / rate,
                 "audio PTS accounts for resampler delay");
    test.equal(avb_decoder_audio_at_eof(decoder), 1, "audio reaches EOF");
    test.equal(avb_decoder_read_audio_f32(decoder, block.data(), 257,
                                         report_pts ? &pts : nullptr),
               0, "repeated read at EOF produces no samples");
    if (report_pts) test.check(pts < 0.0, "EOF has no next timestamp");
  }
  avb_decoder_close(decoder);
}

} // namespace

int main(int argc, char **argv) {
  avb_backend backend = AVB_BACKEND_AUTO;
  if (argc != 3 || !avb::test::parse_backend(argv[2], backend)) return 2;
  avb::test::Context test;
  for (int rate : {22050, 48000})
    for (bool report_pts : {false, true})
      check_resampling(test, argv[1], backend, rate, report_pts);
  return test.finish("audio resampling");
}
