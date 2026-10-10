#include "test.hpp"

#include <limits>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    avb_backend backend;
    if (!avb::test::parse_backend(argv[2], backend)) return 2;
    if (!avb::test::backend_is_built(backend)) return avb::test::skip;
    avb::test::Context test;
    avb_encode_options options = avb_encode_options_default();
    options.backend = backend;
    options.video.enable = 1;
    // Use a size accepted by platform H.264 encoders as well as software codecs.
    constexpr int side = 64;
    options.video.width = options.video.height = side;
    options.video.frame_rate = 30.0;
    options.video.input_format = AVB_PIXEL_FORMAT_BGRA8;
    std::vector<unsigned char> pixels(side * side * 4, 127);
    avb_video_frame frame{};
    frame.width = frame.height = side;
    frame.format = AVB_PIXEL_FORMAT_BGRA8;
    frame.plane_count = 1;
    frame.plane_data[0] = frame.data = pixels.data();
    frame.plane_stride[0] = frame.stride = side * 4;
    frame.data_size = (int)pixels.size();
    float sample = 0.0f;

    test.section("failed open");
    avb_encoder *encoder = nullptr;
    const std::string missing_path = std::string(argv[1]) + ".missing/out.mp4";
    test.check(avb_encoder_open(&encoder, missing_path.c_str(), &options) != AVB_OK,
               "missing parent directory fails open");
    test.check(avb_encoder_get_last_error(encoder) != nullptr, "open failure has a diagnostic");
    test.equal(avb_encoder_write_video(encoder, &frame, 0.0), AVB_ERROR_INVALID_ARGUMENT,
               "failed-open handle rejects video");
    test.equal(avb_encoder_write_audio_f32(encoder, &sample, 1), AVB_ERROR_INVALID_ARGUMENT,
               "failed-open handle rejects audio");
    test.equal(avb_encoder_finish(encoder), AVB_ERROR_INVALID_ARGUMENT,
               "failed-open handle rejects finish");
    avb_encoder_close(encoder);

    test.section("CPU planes and lifecycle");
    if (avb_encoder_open(&encoder, argv[1], &options) != AVB_OK) {
        std::fprintf(stderr, "%s\n", avb::test::encoder_error(encoder));
        avb_encoder_close(encoder);
        return 1;
    }
    auto reject = [&](avb_video_frame bad, const char *message) {
        test.equal(avb_encoder_write_video(encoder, &bad, 0.0), AVB_ERROR_INVALID_ARGUMENT, message);
        test.check(avb_encoder_get_last_error(encoder) != nullptr, "invalid frame has a diagnostic");
    };
    auto bad = frame;
    bad.width++;
    reject(bad, "dimension mismatch rejected");
    bad = frame;
    bad.format = AVB_PIXEL_FORMAT_NV12;
    reject(bad, "format mismatch rejected");
    bad = frame;
    bad.plane_data[0] = nullptr;
    reject(bad, "null plane rejected");
    bad = frame;
    bad.plane_stride[0] = -side * 4;
    reject(bad, "negative stride rejected");
    bad.plane_stride[0] = side * 4 - 1;
    reject(bad, "short stride rejected");
    bad = frame;
    bad.plane_count = AVB_MAX_PLANES + 1;
    reject(bad, "excess planes rejected");
    bad = frame;
    bad.data_size--;
    reject(bad, "short packed buffer rejected");
    bad = frame;
    bad.pts_sec = std::numeric_limits<double>::quiet_NaN();
    reject(bad, "nonfinite timestamp rejected");
    test.equal(avb_encoder_write_video(encoder, &frame, 0.0), AVB_OK,
               "valid frame still writes after invalid inputs");
    test.equal(avb_encoder_finish(encoder), AVB_OK, "valid file finishes");
    test.equal(avb_encoder_write_video(encoder, &frame, 1.0), AVB_ERROR_INVALID_ARGUMENT,
               "finished handle rejects video");
    test.equal(avb_encoder_write_audio_f32(encoder, &sample, 1), AVB_ERROR_INVALID_ARGUMENT,
               "finished handle rejects audio");
    test.equal(avb_encoder_finish(encoder), AVB_ERROR_INVALID_ARGUMENT,
               "finished handle rejects repeated finish");
    avb_encoder_close(encoder);
    std::remove(argv[1]);
    return test.finish("encoder input");
}
