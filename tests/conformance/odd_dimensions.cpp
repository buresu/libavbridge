#include "test.hpp"
#include "avb_plane_layout.hpp"

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    avb_backend backend;
    if (!avb::test::parse_backend(argv[2], backend)) return 2;
    avb::test::Context test;
    // Both tightly packed output and the GStreamer default include the last
    // chroma column/row. GStreamer additionally pads luma to an even height.
    const auto nv12 = avb_plane_layout(AVB_PIXEL_FORMAT_NV12, 65, 49, 1);
    test.equal(nv12.stride[1], 66, "odd NV12 chroma has complete UV pairs");
    test.equal(nv12.rows[1], 25, "odd NV12 includes final chroma row");
    const auto i420 = avb_plane_layout(AVB_PIXEL_FORMAT_I420, 65, 49, 4);
    test.equal(i420.offset[1], size_t(68 * 50), "GStreamer pads luma height");
    test.equal(i420.total, size_t(68 * 50 + 2 * 36 * 25), "GStreamer odd I420 size");
    // Do not run a converter into a known undersized destination in this test.
    if (nv12.rows[1] != 25 || nv12.stride[1] != 66)
        return test.finish("odd dimensions");

    for (auto format : {AVB_PIXEL_FORMAT_BGRA8, AVB_PIXEL_FORMAT_NV12, AVB_PIXEL_FORMAT_I420}) {
        avb_decode_options options = avb_decode_options_default();
        options.backend = backend;
        options.enable_audio = 0;
        options.hardware_policy = AVB_HARDWARE_DISABLED;
        options.video_format = format;
        avb_decoder *decoder = nullptr;
        const auto opened = avb_decoder_open(&decoder, argv[1], &options);
        test.equal(opened, AVB_OK, "odd fixture opens");
        if (opened == AVB_OK) {
            avb_video_frame frame{};
            avb_result result;
            int count = 0;
            while ((result = avb_decoder_read_video_frame(decoder, &frame)) == AVB_OK) {
                test.equal(frame.width, 65, "odd width preserved");
                test.equal(frame.height, 49, "odd height preserved");
                const auto active = avb_plane_layout(format, 65, 49, 1);
                for (int p = 0; p < frame.plane_count; ++p) {
                    const int end = frame.plane_offset[p] +
                        (active.rows[p] - 1) * frame.plane_stride[p] + active.stride[p];
                    test.check(frame.plane_stride[p] >= active.stride[p], "plane row is wide enough");
                    test.check(end <= frame.data_size, "final plane row fits backing buffer");
                    if (end <= frame.data_size && format != AVB_PIXEL_FORMAT_BGRA8) {
                        // The fixture is constant red. Inspect the final chroma row
                        // to catch wrong offsets as well as undersized allocation.
                        const auto *last = frame.plane_data[p] +
                            (active.rows[p] - 1) * frame.plane_stride[p];
                        for (int x = 0; x < active.stride[p]; ++x) {
                            const int expected = p == 0 ? 81 :
                                format == AVB_PIXEL_FORMAT_NV12 ? (x % 2 ? 240 : 90) :
                                p == 1 ? 90 : 240;
                            test.near(last[x], expected, 4, "final row preserves YUV pixels");
                        }
                    }
                }
                ++count;
                avb_decoder_release_video_frame(decoder, &frame);
            }
            test.equal(result, AVB_ERROR_EOF, "odd decode reaches EOS");
            test.equal(count, 3, "all odd frames decode");
        }
        avb_decoder_close(decoder);
    }
    return test.finish("odd dimensions");
}
