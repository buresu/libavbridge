#include "avb_decoder_mediafoundation.hpp"
#include "avb_audio_buffer.hpp"
#include "avb_mediafoundation_common.hpp"
#include "avb_mediafoundation_decode_frames.hpp"
#include "avb_mediafoundation_decode_types.hpp"
#include "avb_mediafoundation_ivf.hpp"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <memory>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

struct AvbDecoderMediaFoundation::Impl {
    struct NativeFrameLease {
        ComPtr<IMFSample> sample;
        ComPtr<ID3D11Texture2D> texture;
    };

    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFTransform> ivf_decoder;
    ComPtr<IMFMediaEventGenerator> ivf_events;
    ComPtr<ID3D11Device> ivf_d3d_device;
    ComPtr<IMFDXGIDeviceManager> ivf_device_manager;
    std::unordered_map<void *, std::unique_ptr<NativeFrameLease>>
        native_frame_leases;
    FILE *ivf_file = nullptr;
    bool ivf_mode = false;
    bool ivf_async = false;
    bool ivf_eof = false;
    bool ivf_draining = false;
    bool ivf_drained = false;
    bool ivf_native_output = false;
    bool source_native_output = false;
    // CPU frames from a hardware decoder: the Source Reader decodes on the GPU
    // and each NV12 surface is copied back through a staging texture. The
    // software decoder is several times slower at UHD, and its RGB conversion
    // slower still.
    bool source_cpu_readback = false;
    ComPtr<ID3D11Texture2D> readback_staging;
    D3D11_TEXTURE2D_DESC readback_desc{};
    uint32_t ivf_frame_count = 0;
    uint32_t ivf_frame_index = 0;
    uint32_t ivf_rate = 0;
    uint32_t ivf_scale = 1;
    long ivf_data_offset = 32;
    DWORD ivf_output_size = 0;
    DWORD ivf_output_flags = 0;
    LONGLONG ivf_pending_pts = 0;
    std::vector<unsigned char> ivf_packet;

    int audio_stream_idx = -1;
    int video_stream_idx = -1;

    int sample_rate  = 0;
    int channels     = 0;
    int audio_track_count = 0;   // selectable audio tracks in the container
    int width        = 0;
    int height       = 0;
    int video_stride = 0;        // bytes per row; may exceed width*4 due to alignment
    bool video_bottom_up = false; // true when MF_MT_DEFAULT_STRIDE is negative
    int video_buffer_height = 0; // padded rows before a planar chroma plane
    avb_color_matrix video_color_matrix = AVB_COLOR_MATRIX_UNKNOWN;
    avb_color_range video_color_range = AVB_COLOR_RANGE_UNKNOWN;

    avb_pixel_format video_avb_fmt = AVB_PIXEL_FORMAT_BGRA8;
    bool swizzle_rgba = false;    // request ARGB32 (BGRA), emit RGBA
    bool video_is_nv12 = false;   // request NV12, emit two planes (Y + CbCr)
    bool video_is_i420 = false;   // request I420, emit three planes (Y + Cb + Cr)

    double duration_sec = 0.0;
    double frame_rate   = 0.0;

    std::string audio_codec_name;
    std::string video_codec_name;

    // Decoded interleaved-float audio waiting to be consumed (drain loop and
    // head-PTS bookkeeping live in AvbAudioBuffer).
    AvbAudioBuffer             audio;
    bool audio_failed = false;
    std::vector<unsigned char> video_frame_buf;
    std::vector<unsigned char> custom_packet_buf;

    const avb_video_decoder_plugin *custom_video_decoder = nullptr;
    void *custom_video_ctx = nullptr;
    bool custom_video = false;

    // After a seek, Media Foundation resumes at the nearest preceding keyframe
    // and does not drop the pre-roll itself, so the first samples carry
    // timestamps before the requested position. Track the target per stream and
    // discard samples until each stream reaches it, matching the AVFoundation
    // backend (which clamps via the reader's time range).
    double seek_target_sec    = 0.0;
    bool   video_seek_pending = false;
    bool   audio_seek_pending = false;

    bool mf_initialized = false;

    void *retain_native_frame(IMFSample *sample, ID3D11Texture2D *texture) {
        auto lease = std::make_unique<NativeFrameLease>();
        lease->sample = sample;
        lease->texture = texture;
        void *key = lease.get();
        native_frame_leases.emplace(key, std::move(lease));
        return key;
    }

    // Copy a hardware-decoded NV12 surface into `out` as tightly packed NV12
    // (Y rows, then interleaved CbCr rows) of width x height. Returns
    // AVB_ERROR_STREAM_NOT_FOUND when the sample is not a GPU surface -- the
    // reader fell back to a software decoder -- so the caller copies it the
    // ordinary way.
    avb_result readback_nv12(IMFSample *sample, int width, int height,
                             std::vector<unsigned char> &out) {
        ComPtr<IMFMediaBuffer> raw;
        ComPtr<IMFDXGIBuffer> dxgi_buffer;
        if (FAILED(sample->GetBufferByIndex(0, &raw)) || !raw ||
            FAILED(raw.As(&dxgi_buffer)) || !dxgi_buffer)
            return AVB_ERROR_STREAM_NOT_FOUND;

        ComPtr<ID3D11Texture2D> texture;
        UINT subresource = 0;
        if (FAILED(dxgi_buffer->GetResource(
                __uuidof(ID3D11Texture2D),
                reinterpret_cast<void **>(texture.GetAddressOf()))) ||
            !texture ||
            FAILED(dxgi_buffer->GetSubresourceIndex(&subresource)))
            return AVB_ERROR_DECODE_FAILED;

        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_NV12 ||
            desc.Width < static_cast<UINT>(width) ||
            desc.Height < static_cast<UINT>(height))
            return AVB_ERROR_DECODE_FAILED;

        ComPtr<ID3D11Device> device;
        texture->GetDevice(&device);
        ComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);

        // One staging surface, kept: its shape only changes with the stream's.
        if (!readback_staging || readback_desc.Width != desc.Width ||
            readback_desc.Height != desc.Height) {
            D3D11_TEXTURE2D_DESC staging = desc;
            staging.MipLevels = 1;
            staging.ArraySize = 1;
            staging.BindFlags = 0;
            staging.MiscFlags = 0;
            staging.Usage = D3D11_USAGE_STAGING;
            staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            readback_staging.Reset();
            if (FAILED(device->CreateTexture2D(&staging, nullptr,
                                               &readback_staging)))
                return AVB_ERROR_DECODE_FAILED;
            readback_desc = staging;
        }

        context->CopySubresourceRegion(readback_staging.Get(), 0, 0, 0, 0,
                                       texture.Get(), subresource, nullptr);
        // Waits for the decode and the copy -- which is the frame being ready.
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(readback_staging.Get(), 0, D3D11_MAP_READ, 0,
                                &mapped)))
            return AVB_ERROR_DECODE_FAILED;

        const size_t chroma_row = static_cast<size_t>((width + 1) / 2) * 2;
        const int chroma_rows = (height + 1) / 2;
        const size_t y_size = static_cast<size_t>(width) * height;
        out.resize(y_size + chroma_row * chroma_rows);
        const auto *source = static_cast<const unsigned char *>(mapped.pData);
        for (int row = 0; row < height; ++row)
            memcpy(out.data() + static_cast<size_t>(row) * width,
                   source + static_cast<size_t>(row) * mapped.RowPitch, width);
        // The chroma plane follows the full (padded) surface height.
        const unsigned char *chroma =
            source + static_cast<size_t>(mapped.RowPitch) * desc.Height;
        for (int row = 0; row < chroma_rows; ++row)
            memcpy(out.data() + y_size + row * chroma_row,
                   chroma + static_cast<size_t>(row) * mapped.RowPitch,
                   chroma_row);
        context->Unmap(readback_staging.Get(), 0);
        return AVB_OK;
    }

    void close_streams() {
        if (custom_video_decoder && custom_video_decoder->close && custom_video_ctx)
            custom_video_decoder->close(custom_video_ctx);
        custom_video_decoder = nullptr;
        custom_video_ctx = nullptr;
        custom_video = false;
        native_frame_leases.clear();
        reader.Reset();
        ivf_events.Reset();
        ivf_decoder.Reset();
        ivf_device_manager.Reset();
        ivf_d3d_device.Reset();
        if (ivf_file) {
            fclose(ivf_file);
            ivf_file = nullptr;
        }
        ivf_mode = false;
        ivf_async = false;
        ivf_eof = false;
        ivf_draining = false;
        ivf_drained = false;
        ivf_native_output = false;
        source_native_output = false;
        source_cpu_readback = false;
        readback_staging.Reset();
        readback_desc = {};
        video_buffer_height = 0;
        video_color_matrix = AVB_COLOR_MATRIX_UNKNOWN;
        video_color_range = AVB_COLOR_RANGE_UNKNOWN;
        ivf_frame_count = 0;
        ivf_frame_index = 0;
        ivf_rate = 0;
        ivf_scale = 1;
        ivf_data_offset = 32;
        ivf_output_size = 0;
        ivf_output_flags = 0;
        ivf_pending_pts = 0;
        ivf_packet.clear();
        audio_stream_idx = video_stream_idx = -1;
        sample_rate = channels = width = height = video_stride = 0;
        audio_track_count = 0;
        video_bottom_up = false;
        video_avb_fmt = AVB_PIXEL_FORMAT_BGRA8;
        swizzle_rgba = false;
        video_is_nv12 = false;
        video_is_i420 = false;
        duration_sec = frame_rate = 0.0;
        audio_codec_name.clear();
        video_codec_name.clear();
        audio.clear();
        audio_failed = false;
        video_frame_buf.clear();
        custom_packet_buf.clear();
        seek_target_sec    = 0.0;
        video_seek_pending = false;
        audio_seek_pending = false;
    }
};

AvbDecoderMediaFoundation::AvbDecoderMediaFoundation() {
    m_impl = new Impl();
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (SUCCEEDED(hr)) {
        m_impl->mf_initialized = true;
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "MFStartup failed: 0x%08lx", hr);
        m_last_error = buf;
    }
}

AvbDecoderMediaFoundation::~AvbDecoderMediaFoundation() {
    if (m_impl) {
        m_impl->close_streams();
        if (m_impl->mf_initialized) MFShutdown();
        delete m_impl;
    }
}

const char *AvbDecoderMediaFoundation::get_backend_name() const { return "mediafoundation"; }
const char *AvbDecoderMediaFoundation::get_last_error() const {
    return m_last_error.empty() ? nullptr : m_last_error.c_str();
}

avb_result AvbDecoderMediaFoundation::open_ivf(
    const char *path, const avb_decode_options &options) {
    if (!options.enable_video) {
        m_last_error = "IVF input contains video only.";
        return AVB_ERROR_STREAM_NOT_FOUND;
    }

    if (options.video_stream_index > 0 ||
        (options.enable_audio && options.audio_stream_index >= 0)) {
        m_last_error = "Requested stream does not exist in IVF input.";
        return AVB_ERROR_STREAM_NOT_FOUND;
    }

    if (options.video_format != AVB_PIXEL_FORMAT_UNKNOWN &&
        options.video_format != AVB_PIXEL_FORMAT_BGRA8 &&
        options.video_format != AVB_PIXEL_FORMAT_RGBA8 &&
        options.video_format != AVB_PIXEL_FORMAT_NV12 &&
        options.video_format != AVB_PIXEL_FORMAT_I420) {
        m_last_error = "Media Foundation IVF decode does not support the requested pixel format.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }

    FILE *file = mf_fopen_utf8(path, L"rb");
    if (!file) {
        m_last_error = "Opening IVF input failed.";
        return AVB_ERROR_OPEN_FAILED;
    }
    MfIvfHeader header{};
    MfIvfReadResult header_result = mf_ivf_read_header(file, header);
    if (header_result == MfIvfReadResult::unsupported) {
        fclose(file);
        m_last_error = "IVF codec is not VP8, VP9, or AV1.";
        return AVB_ERROR_STREAM_NOT_FOUND;
    }
    if (header_result != MfIvfReadResult::ok) {
        fclose(file);
        m_last_error = "Invalid IVF header.";
        return AVB_ERROR_OPEN_FAILED;
    }

    m_impl->width = header.width;
    m_impl->height = header.height;
    m_impl->ivf_rate = header.rate;
    m_impl->ivf_scale = header.scale;
    m_impl->ivf_frame_count = header.frame_count;
    m_impl->ivf_data_offset = header.data_offset;

    GUID input_subtype = mf_ivf_codec_subtype(header.codec);
    MFT_REGISTER_TYPE_INFO decoder_type{
        MFMediaType_Video, input_subtype};
    HRESULT hr = mf_create_transform(
        MFT_CATEGORY_VIDEO_DECODER, &decoder_type, nullptr,
        &m_impl->ivf_decoder, &m_impl->ivf_async);
    if (FAILED(hr) || !m_impl->ivf_decoder) {
        fclose(file);
        char buf[160];
        snprintf(buf, sizeof(buf), "Create IVF decoder MFT failed: 0x%08lx", hr);
        m_last_error = buf;
        return AVB_ERROR_OPEN_FAILED;
    }
    if (m_impl->ivf_async) {
        hr = m_impl->ivf_decoder.As(&m_impl->ivf_events);
        if (FAILED(hr) || !m_impl->ivf_events) {
            fclose(file);
            m_last_error = "Async IVF decoder does not expose IMFMediaEventGenerator.";
            return AVB_ERROR_OPEN_FAILED;
        }
    }
    m_impl->ivf_native_output =
        options.video_memory == AVB_VIDEO_MEMORY_EXTERNAL &&
        options.video_external_type ==
            AVB_VIDEO_EXTERNAL_D3D11_TEXTURE;
    if (m_impl->ivf_native_output) {
        if (options.video_format != AVB_PIXEL_FORMAT_UNKNOWN &&
            options.video_format != AVB_PIXEL_FORMAT_NV12) {
            fclose(file);
            m_last_error = "Media Foundation native IVF decode requires NV12 output.";
            return AVB_ERROR_INVALID_ARGUMENT;
        }
        hr = mf_create_d3d11_device_manager(
            static_cast<ID3D11Device *>(options.hardware_context),
            &m_impl->ivf_d3d_device, &m_impl->ivf_device_manager);
        if (FAILED(hr)) {
            fclose(file);
            char buf[160];
            snprintf(buf, sizeof(buf), "Creating IVF D3D11 device manager failed: 0x%08lx", hr);
            m_last_error = buf;
            return AVB_ERROR_OPEN_FAILED;
        }
        hr = m_impl->ivf_decoder->ProcessMessage(
            MFT_MESSAGE_SET_D3D_MANAGER,
            (ULONG_PTR)m_impl->ivf_device_manager.Get());
        if (FAILED(hr)) {
            fclose(file);
            char buf[160];
            snprintf(buf, sizeof(buf), "Setting IVF D3D11 device manager failed: 0x%08lx", hr);
            m_last_error = buf;
            return AVB_ERROR_OPEN_FAILED;
        }
    }

    hr = mf_ivf_configure_decoder_types(
        m_impl->ivf_decoder.Get(), header,
        &m_impl->ivf_output_size, &m_impl->ivf_output_flags);
    if (FAILED(hr)) {
        fclose(file);
        char buf[160];
        snprintf(
            buf, sizeof(buf),
            "Configuring IVF decoder media types failed: 0x%08lx", hr);
        m_last_error = buf;
        return AVB_ERROR_OPEN_FAILED;
    }

    m_impl->video_avb_fmt =
        m_impl->ivf_native_output ? AVB_PIXEL_FORMAT_NV12 :
        options.video_format == AVB_PIXEL_FORMAT_RGBA8 ? AVB_PIXEL_FORMAT_RGBA8 :
        options.video_format == AVB_PIXEL_FORMAT_NV12 ? AVB_PIXEL_FORMAT_NV12 :
        options.video_format == AVB_PIXEL_FORMAT_I420 ? AVB_PIXEL_FORMAT_I420 :
                                                        AVB_PIXEL_FORMAT_BGRA8;
    m_impl->video_is_nv12 = m_impl->video_avb_fmt == AVB_PIXEL_FORMAT_NV12;
    m_impl->video_is_i420 = m_impl->video_avb_fmt == AVB_PIXEL_FORMAT_I420;
    m_impl->swizzle_rgba = m_impl->video_avb_fmt == AVB_PIXEL_FORMAT_RGBA8;
    m_impl->video_stride = m_impl->width;
    m_impl->frame_rate =
        (double)m_impl->ivf_rate / (double)m_impl->ivf_scale;
    m_impl->duration_sec = m_impl->ivf_frame_count > 0
        ? (double)m_impl->ivf_frame_count / m_impl->frame_rate : 0.0;
    m_impl->video_codec_name = avb_video_codec_name(header.codec);
    m_impl->video_stream_idx = 0;
    m_impl->ivf_file = file;
    m_impl->ivf_mode = true;

    m_impl->ivf_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_impl->ivf_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return AVB_OK;
}

avb_result AvbDecoderMediaFoundation::open_file(const char *path, const avb_decode_options &options) {
    if (!m_impl->mf_initialized) return AVB_ERROR_BACKEND_NOT_AVAILABLE;

    m_impl->close_streams();
    m_last_error.clear();
    size_t path_len = path ? strlen(path) : 0;
    const bool ivf_path =
        path_len >= 4 && _stricmp(path + path_len - 4, ".ivf") == 0;
    const bool native_d3d11 =
        options.video_memory == AVB_VIDEO_MEMORY_EXTERNAL &&
        options.video_external_type ==
            AVB_VIDEO_EXTERNAL_D3D11_TEXTURE &&
        (options.video_format == AVB_PIXEL_FORMAT_UNKNOWN ||
         options.video_format == AVB_PIXEL_FORMAT_NV12) &&
        (options.hardware_device == AVB_HW_DEVICE_AUTO ||
         options.hardware_device == AVB_HW_DEVICE_D3D11VA);
    if (options.video_memory != AVB_VIDEO_MEMORY_CPU && !native_d3d11) {
        m_last_error = "Media Foundation native decode requires D3D11 NV12 output.";
        return AVB_ERROR_OPEN_FAILED;
    }
    if (ivf_path)
        return open_ivf(path, options);

    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (wlen <= 0) {
        m_last_error = "Invalid path encoding.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    std::wstring wpath(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath.data(), wlen);

    // CPU NV12 with hardware allowed: decode on the GPU and read the surfaces
    // back. Only NV12, which is what the decoders produce -- any other CPU
    // format would need the GPU video processor's output read back instead,
    // which is more bytes for a conversion the caller can do better.
    const bool cpu_readback =
        !native_d3d11 && options.enable_video &&
        options.video_memory == AVB_VIDEO_MEMORY_CPU &&
        options.hardware_policy != AVB_HARDWARE_DISABLED &&
        options.video_format == AVB_PIXEL_FORMAT_NV12 &&
        (options.hardware_device == AVB_HW_DEVICE_AUTO ||
         options.hardware_device == AVB_HW_DEVICE_D3D11VA);

    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, (native_d3d11 || cpu_readback) ? 4 : 1);
    // Enables the Video Processor MFT so any codec can be converted to the
    // requested output format regardless of what the decoder natively outputs.
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    if (cpu_readback) {
        // Best effort: without a device the reader simply decodes in software,
        // which is what this open would have done anyway.
        if (SUCCEEDED(mf_create_d3d11_device_manager(
                static_cast<ID3D11Device *>(options.hardware_context),
                &m_impl->ivf_d3d_device, &m_impl->ivf_device_manager)) &&
            SUCCEEDED(attrs->SetUINT32(
                MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE)) &&
            SUCCEEDED(attrs->SetUnknown(
                MF_SOURCE_READER_D3D_MANAGER,
                m_impl->ivf_device_manager.Get()))) {
            m_impl->source_cpu_readback = true;
        }
    }
    if (native_d3d11) {
        HRESULT manager_hr = mf_create_d3d11_device_manager(
            static_cast<ID3D11Device *>(options.hardware_context),
            &m_impl->ivf_d3d_device, &m_impl->ivf_device_manager);
        if (FAILED(manager_hr)) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "Creating Source Reader D3D11 device manager failed: 0x%08lx",
                     manager_hr);
            m_last_error = buf;
            return AVB_ERROR_OPEN_FAILED;
        }
        HRESULT attr_hr =
            attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        if (SUCCEEDED(attr_hr)) {
            attr_hr = attrs->SetUnknown(
                MF_SOURCE_READER_D3D_MANAGER,
                m_impl->ivf_device_manager.Get());
        }
        if (FAILED(attr_hr)) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "Configuring Source Reader D3D11 output failed: 0x%08lx",
                     attr_hr);
            m_last_error = buf;
            return AVB_ERROR_OPEN_FAILED;
        }
        m_impl->source_native_output = true;
    }

    HRESULT hr = MFCreateSourceReaderFromURL(wpath.c_str(), attrs.Get(), &m_impl->reader);
    if (FAILED(hr)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "MFCreateSourceReaderFromURL failed: 0x%08lx", hr);
        m_last_error = buf;
        return AVB_ERROR_OPEN_FAILED;
    }

    int found_audio = -1, found_video = -1, audio_count = 0;
    mf_decode_find_stream_indices(
        m_impl->reader.Get(), &found_audio, &found_video, &audio_count);
    m_impl->audio_track_count = audio_count;

    auto select_stream = [&](bool enabled, int requested, REFGUID major, int &selected) {
        if (!enabled) {
            selected = -1;
            return true;
        }
        if (requested < 0) return true;
        ComPtr<IMFMediaType> type;
        GUID actual = GUID_NULL;
        if (FAILED(m_impl->reader->GetNativeMediaType(requested, 0, &type)) ||
            !type || FAILED(type->GetGUID(MF_MT_MAJOR_TYPE, &actual)) ||
            !IsEqualGUID(actual, major))
            return false;
        selected = requested;
        return true;
    };
    if (!select_stream(options.enable_audio != 0, options.audio_stream_index,
                       MFMediaType_Audio, found_audio) ||
        !select_stream(options.enable_video != 0, options.video_stream_index,
                       MFMediaType_Video, found_video)) {
        m_last_error = "Requested stream does not exist or has the wrong media type.";
        m_impl->close_streams();
        return AVB_ERROR_STREAM_NOT_FOUND;
    }

    m_impl->reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);

    if (found_audio >= 0) {
        std::string native_audio_codec =
            mf_decode_native_codec_name(
                m_impl->reader.Get(), (DWORD)found_audio);
        MfDecodeAudioFormat audio_format{};
        hr = mf_decode_configure_audio(
            m_impl->reader.Get(), (DWORD)found_audio, options,
            &audio_format);
        if (SUCCEEDED(hr)) {
            m_impl->audio_stream_idx = found_audio;
            m_impl->sample_rate = audio_format.sample_rate;
            m_impl->channels = audio_format.channels;
            m_impl->audio_codec_name = native_audio_codec;
        }
    }

    if (found_video >= 0) {
        avb_result custom_res = m_impl->source_native_output
            ? AVB_ERROR_STREAM_NOT_FOUND
            : mf_decode_open_custom_video(
                m_impl->reader.Get(), (DWORD)found_video, options,
                &m_impl->custom_video_decoder, &m_impl->custom_video_ctx,
                m_impl->video_codec_name, &m_impl->width, &m_impl->height,
                &m_impl->frame_rate);
        if (custom_res == AVB_OK) {
            m_impl->reader->SetStreamSelection((DWORD)found_video, TRUE);
            m_impl->video_stream_idx = found_video;
            m_impl->custom_video = true;
        } else if (custom_res != AVB_ERROR_STREAM_NOT_FOUND) {
            m_last_error = "Custom video decoder failed to open.";
            m_impl->close_streams();
            return custom_res;
        } else {
        std::string native_video_codec =
            mf_decode_native_codec_name(
                m_impl->reader.Get(), (DWORD)found_video);
        MfDecodeVideoFormat video_format{};
        hr = mf_decode_configure_video(
            m_impl->reader.Get(), (DWORD)found_video,
            options.video_format, m_impl->source_native_output,
            &video_format);
        if (SUCCEEDED(hr)) {
            m_impl->video_stream_idx = found_video;
            m_impl->video_avb_fmt = video_format.pixel_format;
            m_impl->video_is_nv12 =
                video_format.pixel_format == AVB_PIXEL_FORMAT_NV12;
            m_impl->video_is_i420 =
                video_format.pixel_format == AVB_PIXEL_FORMAT_I420;
            m_impl->swizzle_rgba =
                video_format.pixel_format == AVB_PIXEL_FORMAT_RGBA8;
            m_impl->width = video_format.width;
            m_impl->height = video_format.height;
            m_impl->video_stride = video_format.stride;
            m_impl->video_bottom_up = video_format.bottom_up;
            m_impl->video_buffer_height = video_format.buffer_height;
            m_impl->video_color_matrix = video_format.color_matrix;
            m_impl->video_color_range = video_format.color_range;
            m_impl->frame_rate = video_format.frame_rate;
            m_impl->video_codec_name = native_video_codec;
        }
        }
    }

    if ((options.enable_audio && options.audio_stream_index >= 0 && m_impl->audio_stream_idx < 0) ||
        (options.enable_video && options.video_stream_index >= 0 && m_impl->video_stream_idx < 0) ||
        (m_impl->audio_stream_idx < 0 && m_impl->video_stream_idx < 0)) {
        m_last_error = "No supported audio or video stream found.";
        m_impl->close_streams();
        return AVB_ERROR_STREAM_NOT_FOUND;
    }

    // MF duration is in 100-ns units.
    {
        PROPVARIANT var;
        PropVariantInit(&var);
        if (SUCCEEDED(m_impl->reader->GetPresentationAttribute(
                (DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var))
            && var.vt == VT_UI8) {
            m_impl->duration_sec = (double)var.uhVal.QuadPart / 1e7;
        }
        PropVariantClear(&var);
    }

    return AVB_OK;
}

avb_result AvbDecoderMediaFoundation::get_media_info(avb_media_info &out_info) {
    if (!m_impl->reader && !m_impl->ivf_mode) return AVB_ERROR_INVALID_ARGUMENT;

    out_info = {};
    out_info.backend_name = "mediafoundation";
    out_info.duration_sec = m_impl->duration_sec;

    if (m_impl->audio_stream_idx >= 0) {
        out_info.audio.available    = 1;
        out_info.audio.stream_index = m_impl->audio_stream_idx;
        out_info.audio.track_count  = m_impl->audio_track_count;
        out_info.audio.sample_rate  = m_impl->sample_rate;
        out_info.audio.channels     = m_impl->channels;
        out_info.audio.duration_sec = m_impl->duration_sec;
        out_info.audio.codec_name   = m_impl->audio_codec_name.c_str();
    }

    if (m_impl->video_stream_idx >= 0) {
        out_info.video.available    = 1;
        out_info.video.stream_index = m_impl->video_stream_idx;
        out_info.video.width        = m_impl->width;
        out_info.video.height       = m_impl->height;
        out_info.video.frame_rate   = m_impl->frame_rate;
        out_info.video.duration_sec = m_impl->duration_sec;
        out_info.video.codec_name   = m_impl->video_codec_name.c_str();
    }

    return AVB_OK;
}

avb_result AvbDecoderMediaFoundation::seek(double seconds) {
    if (m_impl->ivf_mode) {
        if (!m_impl->ivf_file || !m_impl->ivf_decoder)
            return AVB_ERROR_INVALID_ARGUMENT;
        if (fseek(m_impl->ivf_file, m_impl->ivf_data_offset, SEEK_SET) != 0) {
            m_last_error = "Seeking IVF input failed.";
            return AVB_ERROR_SEEK_FAILED;
        }
        m_impl->ivf_decoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        m_impl->ivf_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        m_impl->ivf_frame_index = 0;
        m_impl->ivf_eof = false;
        m_impl->ivf_draining = false;
        m_impl->ivf_drained = false;
        m_impl->seek_target_sec = std::max(0.0, seconds);
        m_impl->video_seek_pending = seconds > 0.0;
        return AVB_OK;
    }
    if (!m_impl->reader) return AVB_ERROR_INVALID_ARGUMENT;

    PROPVARIANT var;
    PropVariantInit(&var);
    var.vt            = VT_I8;
    var.hVal.QuadPart = (LONGLONG)(seconds * 1e7);

    HRESULT hr = m_impl->reader->SetCurrentPosition(GUID_NULL, var);
    PropVariantClear(&var);

    if (FAILED(hr)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "SetCurrentPosition failed: 0x%08lx", hr);
        m_last_error = buf;
        return AVB_ERROR_SEEK_FAILED;
    }

    m_impl->audio.clear();
    m_impl->audio_failed = false;
    m_last_error.clear();
    if (m_impl->custom_video_decoder && m_impl->custom_video_decoder->flush)
        m_impl->custom_video_decoder->flush(m_impl->custom_video_ctx);

    // Arm pre-roll dropping for whichever streams are active.
    m_impl->seek_target_sec    = seconds;
    m_impl->video_seek_pending = (m_impl->video_stream_idx >= 0);
    m_impl->audio_seek_pending = (m_impl->audio_stream_idx >= 0);
    return AVB_OK;
}

bool AvbDecoderMediaFoundation::fill_audio_buffer() {
    if (!m_impl || !m_impl->reader || m_impl->audio_stream_idx < 0 ||
        m_impl->channels <= 0 || m_impl->audio_failed)
        return false;

    auto fail = [&](const char *operation, HRESULT hr) {
        char message[160];
        snprintf(message, sizeof(message), "%s (audio) failed: 0x%08lx", operation, hr);
        m_last_error = message;
        m_impl->audio_failed = true;
        return false;
    };

    for (;;) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        HRESULT hr = m_impl->reader->ReadSample(
            (DWORD)m_impl->audio_stream_idx, 0, nullptr, &flags, &ts, &sample);

        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR))
            return fail("ReadSample", FAILED(hr) ? hr : E_FAIL);
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return false;
        if (!sample) continue;

        ComPtr<IMFMediaBuffer> buf;
        hr = sample->ConvertToContiguousBuffer(&buf);
        if (FAILED(hr) || !buf) return fail("ConvertToContiguousBuffer", FAILED(hr) ? hr : E_FAIL);

        BYTE *data = nullptr;
        DWORD len = 0;
        hr = buf->Lock(&data, nullptr, &len);
        if (FAILED(hr)) return fail("Lock", hr);
        if (len % (sizeof(float) * m_impl->channels) != 0 || (len && !data)) {
            buf->Unlock();
            return fail("Invalid PCM buffer", E_FAIL);
        }
        int n = (int)(len / sizeof(float));
        int skip_frames = 0;
        const double sample_pts = static_cast<double>(ts) / 1e7;
        if (m_impl->audio_seek_pending && sample_pts < m_impl->seek_target_sec) {
            const double skip = std::ceil(
                (m_impl->seek_target_sec - sample_pts) * m_impl->sample_rate - 1e-7);
            if (skip >= n / m_impl->channels) {
                buf->Unlock();
                continue;
            }
            skip_frames = static_cast<int>(skip);
        }
        const int skip_samples = skip_frames * m_impl->channels;
        n -= skip_samples;
        // Each MF sample replaces the staging buffer wholesale (pos rewinds to
        // the start), stamped with the sample's presentation time.
        m_impl->audio.data.resize(n);
        if (n > 0)
            memcpy(m_impl->audio.data.data(), data + skip_samples * sizeof(float), n * sizeof(float));
        buf->Unlock();
        m_impl->audio.pos = 0;
        m_impl->audio.pts = sample_pts + static_cast<double>(skip_frames) / m_impl->sample_rate;
        if (n == 0) continue;
        m_impl->audio_seek_pending = false;
        return true;
    }
}

int AvbDecoderMediaFoundation::read_audio_f32(float *dst_interleaved, int frames) {
    if (!m_impl->reader || m_impl->audio_stream_idx < 0 || m_impl->channels <= 0) return 0;
    return m_impl->audio.read(dst_interleaved, frames, m_impl->channels,
                              m_impl->sample_rate,
                              [this] { return fill_audio_buffer(); });
}

double AvbDecoderMediaFoundation::audio_next_pts() {
    if (!m_impl->reader || m_impl->audio_stream_idx < 0 || m_impl->channels <= 0)
        return -1.0;
    return m_impl->audio.next_pts([this] { return fill_audio_buffer(); });
}

bool AvbDecoderMediaFoundation::audio_read_failed() const {
    return m_impl && m_impl->audio_failed;
}

avb_result AvbDecoderMediaFoundation::read_ivf_frame(avb_video_frame &out_frame) {
    if (!m_impl->ivf_file || !m_impl->ivf_decoder)
        return AVB_ERROR_STREAM_NOT_FOUND;
    if (m_impl->ivf_drained) return AVB_ERROR_EOF;

    auto process_output = [&](ComPtr<IMFSample> &decoded) -> avb_result {
retry_output:
        ComPtr<IMFSample> allocated;
        MFT_OUTPUT_DATA_BUFFER output{};
        output.dwStreamID = 0;
        if ((m_impl->ivf_output_flags &
             MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
            ComPtr<IMFMediaBuffer> buffer;
            HRESULT hr = MFCreateMemoryBuffer(m_impl->ivf_output_size, &buffer);
            if (FAILED(hr)) return AVB_ERROR_DECODE_FAILED;
            hr = MFCreateSample(&allocated);
            if (FAILED(hr)) return AVB_ERROR_DECODE_FAILED;
            allocated->AddBuffer(buffer.Get());
            output.pSample = allocated.Get();
        }
        DWORD status = 0;
        HRESULT hr = m_impl->ivf_decoder->ProcessOutput(0, 1, &output, &status);
        if (output.pEvents) output.pEvents->Release();
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            HRESULT type_result = mf_ivf_select_decoder_output(
                m_impl->ivf_decoder.Get(),
                &m_impl->width, &m_impl->height,
                m_impl->ivf_rate, m_impl->ivf_scale,
                &m_impl->ivf_output_size,
                &m_impl->ivf_output_flags);
            if (FAILED(type_result)) {
                m_last_error = "IVF decoder output format change was not NV12.";
                return AVB_ERROR_DECODE_FAILED;
            }
            goto retry_output;
        }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return AVB_ERROR_AGAIN;
        if (FAILED(hr)) {
            char buf[160];
            snprintf(buf, sizeof(buf), "ProcessOutput (IVF decoder) failed: 0x%08lx", hr);
            m_last_error = buf;
            if (output.pSample && output.pSample != allocated.Get())
                output.pSample->Release();
            return AVB_ERROR_DECODE_FAILED;
        }
        IMFSample *sample = output.pSample ? output.pSample : allocated.Get();
        if (sample) sample->AddRef();
        decoded.Attach(sample);
        if (output.pSample && output.pSample != allocated.Get())
            output.pSample->Release();
        return decoded ? AVB_OK : AVB_ERROR_AGAIN;
    };

    // Release each pre-roll sample before requesting another. Recursion here
    // exhausts the stack (and the decoder surface pool) on long seeks.
    ComPtr<IMFSample> decoded;
next_frame:
    decoded.Reset();
    for (;;) {
        if (m_impl->ivf_async) {
            ComPtr<IMFMediaEvent> event;
            HRESULT hr = mf_get_event_with_timeout(
                m_impl->ivf_events.Get(), 10000, &event);
            if (FAILED(hr)) {
                char buf[160];
                snprintf(buf, sizeof(buf), "Waiting for IVF decoder event failed: 0x%08lx", hr);
                m_last_error = buf;
                return AVB_ERROR_DECODE_FAILED;
            }
            HRESULT event_status = S_OK;
            event->GetStatus(&event_status);
            if (FAILED(event_status)) {
                m_last_error = "IVF decoder event reported an error.";
                return AVB_ERROR_DECODE_FAILED;
            }
            MediaEventType type = MEUnknown;
            event->GetType(&type);
            if (type == METransformHaveOutput) {
                avb_result result = process_output(decoded);
                if (result == AVB_OK) break;
                if (result != AVB_ERROR_AGAIN) return result;
                continue;
            }
            if (type == METransformDrainComplete) {
                m_impl->ivf_drained = true;
                return AVB_ERROR_EOF;
            }
            if (type != METransformNeedInput) continue;
        } else if (m_impl->ivf_draining) {
            avb_result result = process_output(decoded);
            if (result == AVB_OK) break;
            if (result == AVB_ERROR_AGAIN) {
                m_impl->ivf_drained = true;
                return AVB_ERROR_EOF;
            }
            return result;
        }

        if (m_impl->ivf_eof) {
            if (!m_impl->ivf_draining) {
                m_impl->ivf_decoder->ProcessMessage(
                    MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
                m_impl->ivf_decoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
                m_impl->ivf_draining = true;
            }
            if (!m_impl->ivf_async) continue;
            continue;
        }

        uint64_t timestamp = 0;
        MfIvfReadResult read_result = mf_ivf_read_frame(
            m_impl->ivf_file, m_impl->ivf_packet, timestamp);
        if (read_result == MfIvfReadResult::eof) {
            m_impl->ivf_eof = true;
            // In async mode this NeedInput event must initiate draining now;
            // there need not be another event until COMMAND_DRAIN is sent.
            m_impl->ivf_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            m_impl->ivf_decoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
            m_impl->ivf_draining = true;
            continue;
        }
        if (read_result != MfIvfReadResult::ok) {
            m_last_error = "Invalid or truncated IVF frame.";
            return AVB_ERROR_DECODE_FAILED;
        }
        uint32_t packet_size =
            static_cast<uint32_t>(m_impl->ivf_packet.size());

        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = MFCreateMemoryBuffer(packet_size, &buffer);
        if (FAILED(hr)) return AVB_ERROR_DECODE_FAILED;
        BYTE *data = nullptr;
        if (FAILED(buffer->Lock(&data, nullptr, nullptr)))
            return AVB_ERROR_DECODE_FAILED;
        memcpy(data, m_impl->ivf_packet.data(), packet_size);
        buffer->Unlock();
        buffer->SetCurrentLength(packet_size);

        ComPtr<IMFSample> input;
        MFCreateSample(&input);
        input->AddBuffer(buffer.Get());
        LONGLONG pts = (LONGLONG)std::llround(
            (double)timestamp * m_impl->ivf_scale * 1e7 / m_impl->ivf_rate);
        LONGLONG duration = (LONGLONG)std::llround(
            (double)m_impl->ivf_scale * 1e7 / m_impl->ivf_rate);
        input->SetSampleTime(pts);
        input->SetSampleDuration(duration);
        m_impl->ivf_pending_pts = pts;
        hr = m_impl->ivf_decoder->ProcessInput(0, input.Get(), 0);
        if (FAILED(hr)) {
            char buf[160];
            snprintf(buf, sizeof(buf), "ProcessInput (IVF decoder) failed: 0x%08lx", hr);
            m_last_error = buf;
            return AVB_ERROR_DECODE_FAILED;
        }
        ++m_impl->ivf_frame_index;
        if (!m_impl->ivf_async) {
            avb_result result = process_output(decoded);
            if (result == AVB_OK) break;
            if (result != AVB_ERROR_AGAIN) return result;
        }
    }

    LONGLONG sample_time = m_impl->ivf_pending_pts;
    decoded->GetSampleTime(&sample_time);
    double pts_sec = (double)sample_time / 1e7;
    if (m_impl->video_seek_pending) {
        if (pts_sec + 1e-6 < m_impl->seek_target_sec)
            goto next_frame;
        m_impl->video_seek_pending = false;
    }

    const int w = m_impl->width;
    const int h = m_impl->height;
    if (m_impl->ivf_native_output) {
        ComPtr<IMFMediaBuffer> raw;
        if (FAILED(decoded->GetBufferByIndex(0, &raw)) || !raw) {
            m_last_error = "Native IVF decoder returned no media buffer.";
            return AVB_ERROR_DECODE_FAILED;
        }
        ComPtr<IMFDXGIBuffer> dxgi_buffer;
        HRESULT hr = raw.As(&dxgi_buffer);
        if (FAILED(hr) || !dxgi_buffer) {
            m_last_error = "Native IVF decoder output is not an IMFDXGIBuffer.";
            return AVB_ERROR_DECODE_FAILED;
        }
        ComPtr<ID3D11Texture2D> texture;
        hr = dxgi_buffer->GetResource(
            __uuidof(ID3D11Texture2D),
            reinterpret_cast<void **>(texture.GetAddressOf()));
        UINT subresource = 0;
        if (SUCCEEDED(hr))
            hr = dxgi_buffer->GetSubresourceIndex(&subresource);
        if (FAILED(hr) || !texture) {
            m_last_error = "Retrieving native IVF D3D11 texture failed.";
            return AVB_ERROR_DECODE_FAILED;
        }

        out_frame = {};
        out_frame.width = w;
        out_frame.height = h;
        out_frame.format = AVB_PIXEL_FORMAT_NV12;
        out_frame.pts_sec = pts_sec;
        out_frame.memory_type = AVB_VIDEO_MEMORY_EXTERNAL;
        out_frame.external_type = AVB_VIDEO_EXTERNAL_D3D11_TEXTURE;
        out_frame.hardware_device = AVB_HW_DEVICE_D3D11VA;
        out_frame.native_handle = texture.Get();
        out_frame.native_handle_id = subresource;
        out_frame.native_owner =
            m_impl->retain_native_frame(decoded.Get(), texture.Get());
        for (int p = 0; p < AVB_MAX_PLANES; ++p) out_frame.dmabuf_fd[p] = -1;
        return AVB_OK;
    }

    const size_t y_size = (size_t)w * h;
    const int chroma_width = (w + 1) / 2;
    const int chroma_height = (h + 1) / 2;
    const int chroma_stride = chroma_width * 2;
    std::vector<unsigned char> nv12;
    avb_video_frame copied_frame{};
    if (mf_decode_copy_cpu_frame(decoded.Get(), w, h, h, chroma_stride,
            false, AVB_PIXEL_FORMAT_NV12, pts_sec, nv12, copied_frame) != AVB_OK) {
        m_last_error = "IVF decoder returned an invalid NV12 frame.";
        return AVB_ERROR_DECODE_FAILED;
    }

    out_frame = {};
    out_frame.width = w;
    out_frame.height = h;
    out_frame.pts_sec = pts_sec;
    out_frame.memory_type = AVB_VIDEO_MEMORY_CPU;
    out_frame.hardware_device = AVB_HW_DEVICE_AUTO;
    for (int p = 0; p < AVB_MAX_PLANES; ++p) out_frame.dmabuf_fd[p] = -1;

    if (m_impl->video_is_nv12) {
        m_impl->video_frame_buf = std::move(nv12);
        out_frame.format = AVB_PIXEL_FORMAT_NV12;
        out_frame.plane_count = 2;
        out_frame.plane_data[0] = m_impl->video_frame_buf.data();
        out_frame.plane_stride[0] = w;
        out_frame.plane_offset[0] = 0;
        out_frame.plane_data[1] = m_impl->video_frame_buf.data() + y_size;
        out_frame.plane_stride[1] = chroma_stride;
        out_frame.plane_offset[1] = (int)y_size;
    } else if (m_impl->video_is_i420) {
        const size_t c_size = static_cast<size_t>(chroma_width) * chroma_height;
        m_impl->video_frame_buf.resize(y_size + c_size * 2);
        memcpy(m_impl->video_frame_buf.data(), nv12.data(), y_size);
        unsigned char *u = m_impl->video_frame_buf.data() + y_size;
        unsigned char *v = u + c_size;
        const unsigned char *uv = nv12.data() + y_size;
        for (size_t i = 0; i < c_size; ++i) {
            u[i] = uv[i * 2];
            v[i] = uv[i * 2 + 1];
        }
        out_frame.format = AVB_PIXEL_FORMAT_I420;
        out_frame.plane_count = 3;
        out_frame.plane_data[0] = m_impl->video_frame_buf.data();
        out_frame.plane_stride[0] = w;
        out_frame.plane_offset[0] = 0;
        out_frame.plane_data[1] = u;
        out_frame.plane_stride[1] = chroma_width;
        out_frame.plane_offset[1] = (int)y_size;
        out_frame.plane_data[2] = v;
        out_frame.plane_stride[2] = chroma_width;
        out_frame.plane_offset[2] = (int)(y_size + c_size);
    } else {
        const int row_bytes = w * 4;
        m_impl->video_frame_buf.resize((size_t)row_bytes * h);
        const unsigned char *y_plane = nv12.data();
        const unsigned char *uv_plane = nv12.data() + y_size;
        for (int y = 0; y < h; ++y) {
            unsigned char *dst =
                m_impl->video_frame_buf.data() + (size_t)y * row_bytes;
            for (int x = 0; x < w; ++x) {
                int yy = std::max(0, (int)y_plane[(size_t)y * w + x] - 16);
                int uu = (int)uv_plane[(size_t)(y / 2) * chroma_stride + (x & ~1)] - 128;
                int vv = (int)uv_plane[(size_t)(y / 2) * chroma_stride + (x & ~1) + 1] - 128;
                int r = (298 * yy + 409 * vv + 128) >> 8;
                int g = (298 * yy - 100 * uu - 208 * vv + 128) >> 8;
                int b = (298 * yy + 516 * uu + 128) >> 8;
                r = std::clamp(r, 0, 255);
                g = std::clamp(g, 0, 255);
                b = std::clamp(b, 0, 255);
                if (m_impl->swizzle_rgba) {
                    dst[x * 4 + 0] = (unsigned char)r;
                    dst[x * 4 + 1] = (unsigned char)g;
                    dst[x * 4 + 2] = (unsigned char)b;
                } else {
                    dst[x * 4 + 0] = (unsigned char)b;
                    dst[x * 4 + 1] = (unsigned char)g;
                    dst[x * 4 + 2] = (unsigned char)r;
                }
                dst[x * 4 + 3] = 255;
            }
        }
        out_frame.format = m_impl->video_avb_fmt;
        out_frame.plane_count = 1;
        out_frame.plane_data[0] = m_impl->video_frame_buf.data();
        out_frame.plane_stride[0] = row_bytes;
        out_frame.plane_offset[0] = 0;
    }
    out_frame.data = out_frame.plane_data[0];
    out_frame.stride = out_frame.plane_stride[0];
    out_frame.data_size = (int)m_impl->video_frame_buf.size();
    return AVB_OK;
}

avb_result AvbDecoderMediaFoundation::read_video_frame(avb_video_frame &out_frame) {
    if (m_impl->ivf_mode) return read_ivf_frame(out_frame);
    if (!m_impl->reader || m_impl->video_stream_idx < 0)
        return AVB_ERROR_STREAM_NOT_FOUND;

    DWORD    flags = 0;
    LONGLONG ts    = 0;
    ComPtr<IMFSample> sample;
    for (;;) {
        flags = 0;
        ts    = 0;
        sample.Reset();
        HRESULT hr = m_impl->reader->ReadSample(
            (DWORD)m_impl->video_stream_idx, 0, nullptr, &flags, &ts, &sample);

        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) {
            char buf[128];
            snprintf(buf, sizeof(buf), "ReadSample (video) failed: 0x%08lx", hr);
            m_last_error = buf;
            return AVB_ERROR_DECODE_FAILED;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return AVB_ERROR_EOF;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            MfDecodeVideoFormat video_format{
                m_impl->video_avb_fmt,
                m_impl->width,
                m_impl->height,
                m_impl->video_stride,
                m_impl->video_bottom_up,
                m_impl->frame_rate,
                m_impl->video_buffer_height,
                m_impl->video_color_matrix,
                m_impl->video_color_range};
            hr = mf_decode_refresh_video_format(
                m_impl->reader.Get(), (DWORD)m_impl->video_stream_idx,
                m_impl->source_native_output, &video_format);
            if (FAILED(hr)) {
                char buf[160];
                snprintf(buf, sizeof(buf),
                         "Updating Source Reader video type failed: 0x%08lx",
                         hr);
                m_last_error = buf;
                return AVB_ERROR_DECODE_FAILED;
            }
            m_impl->width = video_format.width;
            m_impl->height = video_format.height;
            m_impl->video_stride = video_format.stride;
            m_impl->video_bottom_up = video_format.bottom_up;
            m_impl->video_buffer_height = video_format.buffer_height;
            m_impl->video_color_matrix = video_format.color_matrix;
            m_impl->video_color_range = video_format.color_range;
            m_impl->frame_rate = video_format.frame_rate;
        }
        if (!sample) {
            if (m_impl->video_seek_pending ||
                (flags & (MF_SOURCE_READERF_STREAMTICK |
                          MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED |
                          MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED))) {
                continue;
            }
            return AVB_ERROR_DECODE_FAILED;
        }

        // Compressed packets must reach custom decoders, including the seek
        // keyframe and dependencies before the target. Drop their output below.
        if (m_impl->video_seek_pending && !m_impl->custom_video) {
            if ((double)ts / 1e7 + 1e-6 < m_impl->seek_target_sec) continue;
            m_impl->video_seek_pending = false;
        }
        if (m_impl->custom_video) {
            ComPtr<IMFMediaBuffer> buf;
            sample->ConvertToContiguousBuffer(&buf);
            if (!buf) return AVB_ERROR_DECODE_FAILED;

            BYTE *data = nullptr;
            DWORD len = 0;
            if (FAILED(buf->Lock(&data, nullptr, &len))) return AVB_ERROR_DECODE_FAILED;
            m_impl->custom_packet_buf.resize(len);
            memcpy(m_impl->custom_packet_buf.data(), data, len);
            buf->Unlock();

            LONGLONG dur = 0;
            sample->GetSampleDuration(&dur);
            UINT32 clean_point = 0;
            sample->GetUINT32(MFSampleExtension_CleanPoint, &clean_point);
            UINT64 decode_time = static_cast<UINT64>(ts);
            sample->GetUINT64(MFSampleExtension_DecodeTimestamp, &decode_time);

            avb_encoded_packet packet{};
            packet.data = m_impl->custom_packet_buf.data();
            packet.size = (int)m_impl->custom_packet_buf.size();
            packet.pts_sec = (double)ts / 1e7;
            packet.duration_sec = dur > 0 ? (double)dur / 1e7 : 0.0;
            packet.keyframe = clean_point ? 1 : 0;
            packet.stream_index = m_impl->video_stream_idx;
            packet.pts = ts;
            packet.dts = static_cast<int64_t>(decode_time);
            packet.duration = dur;
            packet.time_base_num = 1;
            packet.time_base_den = 10000000;

            avb_result res = m_impl->custom_video_decoder->decode_packet(
                m_impl->custom_video_ctx, &packet, &out_frame);
            if (res == AVB_ERROR_AGAIN) continue;
            if (res == AVB_OK && out_frame.pts_sec < 0.0)
                out_frame.pts_sec = packet.pts_sec;
            if (res == AVB_OK && m_impl->video_seek_pending) {
                if (out_frame.pts_sec + 1e-6 < m_impl->seek_target_sec) {
                    release_video_frame(out_frame);
                    out_frame = {};
                    continue;
                }
                m_impl->video_seek_pending = false;
            }
            return res;
        }
        break;
    }

    const int w = m_impl->width;
    const int h = m_impl->height;

    if (m_impl->source_native_output) {
        ComPtr<IMFMediaBuffer> raw;
        if (FAILED(sample->GetBufferByIndex(0, &raw)) || !raw) {
            m_last_error = "Native Source Reader output contains no media buffer.";
            return AVB_ERROR_DECODE_FAILED;
        }
        ComPtr<IMFDXGIBuffer> dxgi_buffer;
        HRESULT hr = raw.As(&dxgi_buffer);
        if (FAILED(hr) || !dxgi_buffer) {
            m_last_error = "Native Source Reader output is not an IMFDXGIBuffer.";
            return AVB_ERROR_DECODE_FAILED;
        }
        ComPtr<ID3D11Texture2D> texture;
        hr = dxgi_buffer->GetResource(
            __uuidof(ID3D11Texture2D),
            reinterpret_cast<void **>(texture.GetAddressOf()));
        UINT subresource = 0;
        if (SUCCEEDED(hr))
            hr = dxgi_buffer->GetSubresourceIndex(&subresource);
        if (FAILED(hr) || !texture) {
            m_last_error = "Retrieving Source Reader D3D11 texture failed.";
            return AVB_ERROR_DECODE_FAILED;
        }

        out_frame = {};
        out_frame.width = w;
        out_frame.height = h;
        out_frame.format = AVB_PIXEL_FORMAT_NV12;
        out_frame.pts_sec = (double)ts / 1e7;
        out_frame.memory_type = AVB_VIDEO_MEMORY_EXTERNAL;
        out_frame.external_type = AVB_VIDEO_EXTERNAL_D3D11_TEXTURE;
        out_frame.hardware_device = AVB_HW_DEVICE_D3D11VA;
        out_frame.native_handle = texture.Get();
        out_frame.native_handle_id = subresource;
        out_frame.native_owner =
            m_impl->retain_native_frame(sample.Get(), texture.Get());
        out_frame.color_matrix = m_impl->video_color_matrix;
        out_frame.color_range = m_impl->video_color_range;
        for (int p = 0; p < AVB_MAX_PLANES; ++p) out_frame.dmabuf_fd[p] = -1;
        return AVB_OK;
    }

    const double pts_sec = static_cast<double>(ts) / 1e7;
    avb_result res = AVB_ERROR_STREAM_NOT_FOUND;
    if (m_impl->source_cpu_readback && m_impl->video_is_nv12) {
        res = m_impl->readback_nv12(sample.Get(), w, h, m_impl->video_frame_buf);
        if (res == AVB_OK) {
            const size_t y_size = static_cast<size_t>(w) * h;
            const int chroma_row = (w + 1) / 2 * 2;
            out_frame = {};
            out_frame.width = w;
            out_frame.height = h;
            out_frame.format = AVB_PIXEL_FORMAT_NV12;
            out_frame.pts_sec = pts_sec;
            out_frame.memory_type = AVB_VIDEO_MEMORY_CPU;
            out_frame.hardware_device = AVB_HW_DEVICE_D3D11VA;
            for (int p = 0; p < AVB_MAX_PLANES; ++p) out_frame.dmabuf_fd[p] = -1;
            out_frame.plane_count = 2;
            out_frame.plane_data[0] = m_impl->video_frame_buf.data();
            out_frame.plane_stride[0] = w;
            out_frame.plane_offset[0] = 0;
            out_frame.plane_data[1] = m_impl->video_frame_buf.data() + y_size;
            out_frame.plane_stride[1] = chroma_row;
            out_frame.plane_offset[1] = static_cast<int>(y_size);
            out_frame.data = out_frame.plane_data[0];
            out_frame.stride = out_frame.plane_stride[0];
            out_frame.data_size =
                static_cast<int>(m_impl->video_frame_buf.size());
        } else if (res != AVB_ERROR_STREAM_NOT_FOUND) {
            m_last_error = "Reading back a hardware-decoded frame failed.";
            return res;
        }
    }
    // Not a GPU surface (software decode, or a format the readback does not
    // cover): copy it out of system memory.
    if (res == AVB_ERROR_STREAM_NOT_FOUND) {
        res = mf_decode_copy_cpu_frame(
            sample.Get(),
            w,
            h,
            m_impl->video_buffer_height > 0 ? m_impl->video_buffer_height : h,
            m_impl->video_stride,
            m_impl->video_bottom_up,
            m_impl->video_avb_fmt,
            pts_sec,
            m_impl->video_frame_buf,
            out_frame);
    }
    if (res == AVB_OK) {
        out_frame.color_matrix = m_impl->video_color_matrix;
        out_frame.color_range = m_impl->video_color_range;
    }
    return res;
}

void AvbDecoderMediaFoundation::release_video_frame(avb_video_frame &frame) {
    if (m_impl && frame.memory_type == AVB_VIDEO_MEMORY_EXTERNAL &&
        frame.external_type == AVB_VIDEO_EXTERNAL_D3D11_TEXTURE &&
        frame.native_owner) {
        auto lease = m_impl->native_frame_leases.find(frame.native_owner);
        if (lease != m_impl->native_frame_leases.end()) {
            m_impl->native_frame_leases.erase(lease);
            return;
        }
    }
    if (m_impl && m_impl->custom_video_decoder) {
        if (m_impl->custom_video_decoder->release_frame)
            m_impl->custom_video_decoder->release_frame(m_impl->custom_video_ctx, &frame);
        return;
    }
}

#else // !_WIN32

AvbDecoderMediaFoundation::AvbDecoderMediaFoundation() {
    m_impl = nullptr;
    m_last_error = "Media Foundation backend is only available on Windows.";
}
AvbDecoderMediaFoundation::~AvbDecoderMediaFoundation() {}
const char *AvbDecoderMediaFoundation::get_backend_name() const { return "mediafoundation"; }
const char *AvbDecoderMediaFoundation::get_last_error() const {
    return m_last_error.empty() ? nullptr : m_last_error.c_str();
}
avb_result AvbDecoderMediaFoundation::open_file(const char *, const avb_decode_options &) {
    return AVB_ERROR_BACKEND_NOT_AVAILABLE;
}
avb_result AvbDecoderMediaFoundation::get_media_info(avb_media_info &) {
    return AVB_ERROR_BACKEND_NOT_AVAILABLE;
}
avb_result AvbDecoderMediaFoundation::seek(double) { return AVB_ERROR_BACKEND_NOT_AVAILABLE; }
int AvbDecoderMediaFoundation::read_audio_f32(float *, int) { return 0; }
double AvbDecoderMediaFoundation::audio_next_pts() { return -1.0; }
bool AvbDecoderMediaFoundation::audio_read_failed() const { return false; }
avb_result AvbDecoderMediaFoundation::read_video_frame(avb_video_frame &) {
    return AVB_ERROR_BACKEND_NOT_AVAILABLE;
}
void AvbDecoderMediaFoundation::release_video_frame(avb_video_frame &) {}

#endif // _WIN32
