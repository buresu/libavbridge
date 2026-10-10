#include "test.hpp"
#include "backends/mediafoundation/avb_mediafoundation_decode_frames.hpp"
#include <mfapi.h>
#include <wrl/client.h>
#include <vector>

using Microsoft::WRL::ComPtr;

int main() {
    avb::test::Context test;
    if (FAILED(MFStartup(MF_VERSION))) return avb::test::skip;
    for (auto format : {AVB_PIXEL_FORMAT_NV12, AVB_PIXEL_FORMAT_I420, AVB_PIXEL_FORMAT_BGRA8}) {
        // Odd visible dimensions within a padded media buffer.
        const int width = 65, height = 49, buffer_height = 50;
        const int stride = format == AVB_PIXEL_FORMAT_BGRA8 ? 272 : 68;
        const int size = format == AVB_PIXEL_FORMAT_BGRA8 ? stride * height : stride * buffer_height * 3 / 2;
        ComPtr<IMFMediaBuffer> buffer;
        ComPtr<IMFSample> sample;
        test.equal(MFCreateMemoryBuffer(size, &buffer), S_OK, "allocate media buffer");
        test.equal(MFCreateSample(&sample), S_OK, "allocate sample");
        if (!buffer || !sample) continue;
        BYTE *data = nullptr;
        buffer->Lock(&data, nullptr, nullptr);
        std::memset(data, 81, size);
        if (format == AVB_PIXEL_FORMAT_NV12) {
            for (int i = stride * buffer_height; i < size; ++i)
                data[i] = (i % 2) ? 240 : 90;
        } else if (format == AVB_PIXEL_FORMAT_I420) {
            std::memset(data + stride * buffer_height, 90, stride * buffer_height / 4);
            std::memset(data + stride * buffer_height * 5 / 4, 240, stride * buffer_height / 4);
        }
        buffer->Unlock();
        buffer->SetCurrentLength(size);
        sample->AddBuffer(buffer.Get());
        std::vector<unsigned char> storage;
        avb_video_frame frame{};
        test.equal(mf_decode_copy_cpu_frame(sample.Get(), width, height, buffer_height,
            stride, false, format, 0.0, storage, frame), AVB_OK, "padded odd frame copies");
        const int expected_size = format == AVB_PIXEL_FORMAT_BGRA8 ? width * height * 4 : width * height + 66 * 25;
        test.equal(frame.data_size, expected_size, "odd frame retains full chroma dimensions");
        if (frame.data_size == expected_size && format != AVB_PIXEL_FORMAT_BGRA8) {
            test.equal(storage.back(), static_cast<unsigned char>(240), "last chroma pixel is preserved");
            test.equal(frame.plane_stride[1], format == AVB_PIXEL_FORMAT_NV12 ? 66 : 33,
                       "odd chroma stride rounds up");
        }
        buffer->SetCurrentLength(1);
        test.equal(mf_decode_copy_cpu_frame(sample.Get(), width, height, buffer_height,
            stride, false, format, 0.0, storage, frame), AVB_ERROR_DECODE_FAILED,
            "truncated frame is rejected before copying");
        buffer->SetCurrentLength(size);
        test.equal(mf_decode_copy_cpu_frame(sample.Get(), width, height, buffer_height,
            1, false, format, 0.0, storage, frame), AVB_ERROR_DECODE_FAILED,
            "undersized stride is rejected");
    }
    MFShutdown();
    return test.finish("Media Foundation frame layout");
}
