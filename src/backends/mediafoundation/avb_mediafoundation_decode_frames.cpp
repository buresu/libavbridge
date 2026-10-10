#include "avb_mediafoundation_decode_frames.hpp"

#ifdef _WIN32

#include <wrl/client.h>

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {

void initialize_cpu_frame(
    avb_video_frame &frame,
    int width,
    int height,
    avb_pixel_format format,
    double pts_sec) {
    frame = {};
    frame.width = width;
    frame.height = height;
    frame.format = format;
    frame.pts_sec = pts_sec;
    frame.memory_type = AVB_VIDEO_MEMORY_CPU;
    frame.hardware_device = AVB_HW_DEVICE_AUTO;
    for (int plane = 0; plane < AVB_MAX_PLANES; ++plane)
        frame.dmabuf_fd[plane] = -1;
}

bool copy_nv12(
    IMFSample *sample,
    int width,
    int height,
    int buffer_height,
    int source_stride,
    unsigned char *output) {
    const int chroma_rows = (height + 1) / 2;
    const int chroma_row_bytes = (width + 1) / 2 * 2;
    const std::size_t y_size =
        static_cast<std::size_t>(width) * height;

    DWORD buffer_count = 0;
    sample->GetBufferCount(&buffer_count);
    if (buffer_count == 1) {
        ComPtr<IMFMediaBuffer> raw;
        sample->GetBufferByIndex(0, &raw);

        ComPtr<IMF2DBuffer> buffer2d;
        if (raw && SUCCEEDED(raw.As(&buffer2d))) {
            BYTE *scan0 = nullptr;
            LONG pitch = 0;
            if (SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch))) {
                if (!scan0 || pitch < chroma_row_bytes) {
                    buffer2d->Unlock2D();
                    return false;
                }
                const BYTE *chroma =
                    scan0 + static_cast<ptrdiff_t>(pitch) * buffer_height;
                for (int row = 0; row < height; ++row) {
                    std::memcpy(
                        output + static_cast<std::size_t>(row) * width,
                        scan0 + static_cast<ptrdiff_t>(row) * pitch,
                        width);
                }
                for (int row = 0; row < chroma_rows; ++row) {
                    std::memcpy(
                        output + y_size +
                            static_cast<std::size_t>(row) * chroma_row_bytes,
                        chroma + static_cast<ptrdiff_t>(row) * pitch,
                        chroma_row_bytes);
                }
                buffer2d->Unlock2D();
                return true;
            }
        }
    }

    ComPtr<IMFMediaBuffer> buffer;
    sample->ConvertToContiguousBuffer(&buffer);
    if (!buffer) return false;

    BYTE *data = nullptr;
    DWORD length = 0;
    if (FAILED(buffer->Lock(&data, nullptr, &length))) return false;
    const int stride = source_stride > 0 ? source_stride : chroma_row_bytes;
    const size_t required = static_cast<size_t>(stride) * buffer_height +
        static_cast<size_t>(stride) * (chroma_rows - 1) + chroma_row_bytes;
    if (!data || stride < chroma_row_bytes || length < required) {
        buffer->Unlock();
        return false;
    }
    const BYTE *chroma =
        data + static_cast<std::size_t>(stride) * buffer_height;
    for (int row = 0; row < height; ++row) {
        std::memcpy(
            output + static_cast<std::size_t>(row) * width,
            data + static_cast<std::size_t>(row) * stride,
            width);
    }
    for (int row = 0; row < chroma_rows; ++row) {
        std::memcpy(
            output + y_size + static_cast<std::size_t>(row) * chroma_row_bytes,
            chroma + static_cast<std::size_t>(row) * stride,
            chroma_row_bytes);
    }
    buffer->Unlock();
    return true;
}

bool copy_i420(
    IMFSample *sample,
    int width,
    int height,
    int buffer_height,
    int source_stride,
    unsigned char *output) {
    const int chroma_width = (width + 1) / 2;
    const int chroma_height = (height + 1) / 2;
    const std::size_t y_size =
        static_cast<std::size_t>(width) * height;
    const std::size_t chroma_size =
        static_cast<std::size_t>(chroma_width) * chroma_height;
    unsigned char *output_u = output + y_size;
    unsigned char *output_v = output_u + chroma_size;

    auto copy_planes = [&](const BYTE *source_y, int y_pitch) {
        const int chroma_pitch = (y_pitch + 1) / 2;
        const BYTE *source_u =
            source_y + static_cast<ptrdiff_t>(y_pitch) * buffer_height;
        const BYTE *source_v =
            source_u +
            static_cast<ptrdiff_t>(chroma_pitch) * ((buffer_height + 1) / 2);
        for (int row = 0; row < height; ++row) {
            std::memcpy(
                output + static_cast<std::size_t>(row) * width,
                source_y + static_cast<ptrdiff_t>(row) * y_pitch,
                width);
        }
        for (int row = 0; row < chroma_height; ++row) {
            std::memcpy(
                output_u +
                    static_cast<std::size_t>(row) * chroma_width,
                source_u +
                    static_cast<ptrdiff_t>(row) * chroma_pitch,
                chroma_width);
            std::memcpy(
                output_v +
                    static_cast<std::size_t>(row) * chroma_width,
                source_v +
                    static_cast<ptrdiff_t>(row) * chroma_pitch,
                chroma_width);
        }
    };

    DWORD buffer_count = 0;
    sample->GetBufferCount(&buffer_count);
    if (buffer_count == 1) {
        ComPtr<IMFMediaBuffer> raw;
        sample->GetBufferByIndex(0, &raw);

        ComPtr<IMF2DBuffer> buffer2d;
        if (raw && SUCCEEDED(raw.As(&buffer2d))) {
            BYTE *scan0 = nullptr;
            LONG pitch = 0;
            if (SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch))) {
                if (!scan0 || pitch < width) {
                    buffer2d->Unlock2D();
                    return false;
                }
                copy_planes(scan0, static_cast<int>(pitch));
                buffer2d->Unlock2D();
                return true;
            }
        }
    }

    ComPtr<IMFMediaBuffer> buffer;
    sample->ConvertToContiguousBuffer(&buffer);
    if (!buffer) return false;

    BYTE *data = nullptr;
    DWORD length = 0;
    if (FAILED(buffer->Lock(&data, nullptr, &length))) return false;
    const int stride = source_stride > 0 ? source_stride : width;
    const int chroma_stride = (stride + 1) / 2;
    const size_t required = static_cast<size_t>(stride) * buffer_height +
        static_cast<size_t>(chroma_stride) * ((buffer_height + 1) / 2) +
        static_cast<size_t>(chroma_stride) * (chroma_height - 1) + chroma_width;
    if (!data || stride < width || length < required) {
        buffer->Unlock();
        return false;
    }
    copy_planes(data, stride);
    buffer->Unlock();
    return true;
}

bool copy_packed(
    IMFSample *sample,
    int width,
    int height,
    int source_stride,
    bool bottom_up,
    unsigned char *output) {
    const int row_bytes = width * 4;
    DWORD buffer_count = 0;
    sample->GetBufferCount(&buffer_count);
    if (buffer_count == 1) {
        ComPtr<IMFMediaBuffer> raw;
        sample->GetBufferByIndex(0, &raw);

        ComPtr<IMF2DBuffer> buffer2d;
        if (raw && SUCCEEDED(raw.As(&buffer2d))) {
            BYTE *scan0 = nullptr;
            LONG pitch = 0;
            if (SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch))) {
                if (!scan0 || (pitch > -row_bytes && pitch < row_bytes)) {
                    buffer2d->Unlock2D();
                    return false;
                }
                for (int row = 0; row < height; ++row) {
                    std::memcpy(
                        output +
                            static_cast<std::size_t>(row) * row_bytes,
                        scan0 + static_cast<ptrdiff_t>(row) * pitch,
                        row_bytes);
                }
                buffer2d->Unlock2D();
                return true;
            }
        }
    }

    ComPtr<IMFMediaBuffer> buffer;
    sample->ConvertToContiguousBuffer(&buffer);
    if (!buffer) return false;

    BYTE *data = nullptr;
    DWORD length = 0;
    if (FAILED(buffer->Lock(&data, nullptr, &length))) return false;
    const int stride = source_stride > 0 ? source_stride : row_bytes;
    const size_t required = static_cast<size_t>(stride) * (height - 1) + row_bytes;
    if (!data || stride < row_bytes || length < required) {
        buffer->Unlock();
        return false;
    }
    for (int row = 0; row < height; ++row) {
        const int source_row = bottom_up ? height - 1 - row : row;
        std::memcpy(
            output + static_cast<std::size_t>(row) * row_bytes,
            data + static_cast<std::size_t>(source_row) * stride,
            row_bytes);
    }
    buffer->Unlock();
    return true;
}

} // namespace

avb_result mf_decode_copy_cpu_frame(
    IMFSample *sample,
    int width,
    int height,
    int buffer_height,
    int source_stride,
    bool bottom_up,
    avb_pixel_format output_format,
    double pts_sec,
    std::vector<unsigned char> &storage,
    avb_video_frame &output) {
    if (!sample || width <= 0 || height <= 0 || buffer_height < height)
        return AVB_ERROR_DECODE_FAILED;

    if (output_format == AVB_PIXEL_FORMAT_NV12) {
        const std::size_t y_size =
            static_cast<std::size_t>(width) * height;
        const int chroma_row_bytes = (width + 1) / 2 * 2;
        storage.resize(y_size + static_cast<size_t>(chroma_row_bytes) * ((height + 1) / 2));
        if (!copy_nv12(
                sample, width, height, buffer_height, source_stride,
                storage.data())) {
            return AVB_ERROR_DECODE_FAILED;
        }

        initialize_cpu_frame(
            output, width, height, output_format, pts_sec);
        output.plane_count = 2;
        output.plane_data[0] = storage.data();
        output.plane_stride[0] = width;
        output.plane_offset[0] = 0;
        output.plane_data[1] = storage.data() + y_size;
        output.plane_stride[1] = chroma_row_bytes;
        output.plane_offset[1] = static_cast<int>(y_size);
    } else if (output_format == AVB_PIXEL_FORMAT_I420) {
        const int chroma_width = (width + 1) / 2;
        const int chroma_height = (height + 1) / 2;
        const std::size_t y_size =
            static_cast<std::size_t>(width) * height;
        const std::size_t chroma_size =
            static_cast<std::size_t>(chroma_width) * chroma_height;
        storage.resize(y_size + 2 * chroma_size);
        if (!copy_i420(
                sample, width, height, buffer_height, source_stride,
                storage.data())) {
            return AVB_ERROR_DECODE_FAILED;
        }

        initialize_cpu_frame(
            output, width, height, output_format, pts_sec);
        output.plane_count = 3;
        output.plane_data[0] = storage.data();
        output.plane_stride[0] = width;
        output.plane_offset[0] = 0;
        output.plane_data[1] = storage.data() + y_size;
        output.plane_stride[1] = chroma_width;
        output.plane_offset[1] = static_cast<int>(y_size);
        output.plane_data[2] =
            storage.data() + y_size + chroma_size;
        output.plane_stride[2] = chroma_width;
        output.plane_offset[2] =
            static_cast<int>(y_size + chroma_size);
    } else {
        const int row_bytes = width * 4;
        storage.resize(static_cast<std::size_t>(row_bytes) * height);
        if (!copy_packed(
                sample, width, height, source_stride, bottom_up,
                storage.data())) {
            return AVB_ERROR_DECODE_FAILED;
        }

        if (output_format == AVB_PIXEL_FORMAT_RGBA8) {
            for (int pixel = 0; pixel < width * height; ++pixel) {
                unsigned char blue = storage[pixel * 4];
                storage[pixel * 4] = storage[pixel * 4 + 2];
                storage[pixel * 4 + 2] = blue;
            }
        }

        initialize_cpu_frame(
            output, width, height, output_format, pts_sec);
        output.plane_count = 1;
        output.plane_data[0] = storage.data();
        output.plane_stride[0] = row_bytes;
        output.plane_offset[0] = 0;
    }

    output.data = output.plane_data[0];
    output.stride = output.plane_stride[0];
    output.data_size = static_cast<int>(storage.size());
    return AVB_OK;
}

#endif
