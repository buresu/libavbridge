#include "avb_backend_mediafoundation.hpp"

#ifdef _WIN32

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>
#include <windows.h>

#include <cstring>
#include <cstdio>
#include <vector>

using Microsoft::WRL::ComPtr;

struct AvbBackendMediaFoundation::Impl {
    ComPtr<IMFSourceReader> reader;

    int audio_stream_idx = -1;
    int video_stream_idx = -1;

    int sample_rate  = 0;
    int channels     = 0;
    int width        = 0;
    int height       = 0;
    double duration_sec = 0.0;
    double frame_rate   = 0.0;

    std::string audio_codec_name;
    std::string video_codec_name;

    std::vector<float> audio_buf;
    int audio_buf_pos = 0;

    std::vector<unsigned char> video_frame_buf;

    bool mf_initialized = false;
};

AvbBackendMediaFoundation::AvbBackendMediaFoundation() {
    m_impl = new Impl();
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (SUCCEEDED(hr)) {
        m_impl->mf_initialized = true;
    } else {
        m_last_error = "MFStartup failed.";
    }
}

AvbBackendMediaFoundation::~AvbBackendMediaFoundation() {
    if (m_impl) {
        if (m_impl->mf_initialized) MFShutdown();
        delete m_impl;
    }
}

const char *AvbBackendMediaFoundation::get_backend_name() const { return "mediafoundation"; }
const char *AvbBackendMediaFoundation::get_last_error() const {
    return m_last_error.empty() ? nullptr : m_last_error.c_str();
}

avb_result AvbBackendMediaFoundation::open_file(const char *path, const avb_open_options &options) {
    if (!m_impl->mf_initialized) return AVB_ERROR_BACKEND_NOT_AVAILABLE;

    // Convert path to wide string
    int len = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    std::wstring wpath(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath.data(), len);

    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 1);
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);

    HRESULT hr = MFCreateSourceReaderFromURL(wpath.c_str(), attrs.Get(), &m_impl->reader);
    if (FAILED(hr)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "MFCreateSourceReaderFromURL failed: 0x%08lx", hr);
        m_last_error = buf;
        return AVB_ERROR_OPEN_FAILED;
    }

    // Configure audio output type: PCM float
    if (options.enable_audio) {
        ComPtr<IMFMediaType> audio_type;
        MFCreateMediaType(&audio_type);
        audio_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        audio_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);

        hr = m_impl->reader->SetCurrentMediaType(
            (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, audio_type.Get());
        if (SUCCEEDED(hr)) {
            m_impl->audio_stream_idx = (int)MF_SOURCE_READER_FIRST_AUDIO_STREAM;

            ComPtr<IMFMediaType> current_type;
            m_impl->reader->GetCurrentMediaType(
                (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &current_type);
            if (current_type) {
                UINT32 sr = 0, ch = 0;
                current_type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
                current_type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
                m_impl->sample_rate = (int)sr;
                m_impl->channels    = (int)ch;
            }
            m_impl->audio_codec_name = "pcm_f32";
        }
    }

    // Configure video output type: BGRA
    if (options.enable_video) {
        ComPtr<IMFMediaType> video_type;
        MFCreateMediaType(&video_type);
        video_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        video_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);

        hr = m_impl->reader->SetCurrentMediaType(
            (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, video_type.Get());
        if (SUCCEEDED(hr)) {
            m_impl->video_stream_idx = (int)MF_SOURCE_READER_FIRST_VIDEO_STREAM;

            ComPtr<IMFMediaType> current_type;
            m_impl->reader->GetCurrentMediaType(
                (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current_type);
            if (current_type) {
                UINT32 w = 0, h = 0;
                MFGetAttributeSize(current_type.Get(), MF_MT_FRAME_SIZE, &w, &h);
                m_impl->width  = (int)w;
                m_impl->height = (int)h;

                UINT32 num = 0, den = 1;
                MFGetAttributeRatio(current_type.Get(), MF_MT_FRAME_RATE, &num, &den);
                if (den != 0) m_impl->frame_rate = (double)num / den;
            }
            m_impl->video_codec_name = "bgra";
        }
    }

    // Get duration
    PROPVARIANT var;
    PropVariantInit(&var);
    hr = m_impl->reader->GetPresentationAttribute(
        (DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var);
    if (SUCCEEDED(hr) && var.vt == VT_UI8) {
        m_impl->duration_sec = (double)var.uhVal.QuadPart / 1e7;
    }
    PropVariantClear(&var);

    if (m_impl->audio_stream_idx < 0 && m_impl->video_stream_idx < 0) {
        m_last_error = "No supported audio or video stream found.";
        return AVB_ERROR_STREAM_NOT_FOUND;
    }

    return AVB_OK;
}

avb_result AvbBackendMediaFoundation::get_media_info(avb_media_info &out_info) {
    if (!m_impl->reader) return AVB_ERROR_INVALID_ARGUMENT;

    out_info = {};
    out_info.backend_name  = "mediafoundation";
    out_info.duration_sec  = m_impl->duration_sec;

    if (m_impl->audio_stream_idx >= 0) {
        out_info.audio.available    = 1;
        out_info.audio.stream_index = m_impl->audio_stream_idx;
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

avb_result AvbBackendMediaFoundation::seek(double seconds) {
    if (!m_impl->reader) return AVB_ERROR_INVALID_ARGUMENT;

    PROPVARIANT var;
    PropVariantInit(&var);
    var.vt = VT_I8;
    var.hVal.QuadPart = (LONGLONG)(seconds * 1e7);

    HRESULT hr = m_impl->reader->SetCurrentPosition(GUID_NULL, var);
    PropVariantClear(&var);

    if (FAILED(hr)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "SetCurrentPosition failed: 0x%08lx", hr);
        m_last_error = buf;
        return AVB_ERROR_SEEK_FAILED;
    }

    m_impl->audio_buf.clear();
    m_impl->audio_buf_pos = 0;
    return AVB_OK;
}

int AvbBackendMediaFoundation::read_audio_f32(float *dst_interleaved, int frames) {
    if (!m_impl->reader || m_impl->audio_stream_idx < 0) return 0;

    int nb_channels   = m_impl->channels;
    int samples_needed = frames * nb_channels;
    int samples_written = 0;

    while (samples_written < samples_needed) {
        int available = (int)m_impl->audio_buf.size() - m_impl->audio_buf_pos;
        if (available > 0) {
            int to_copy = samples_needed - samples_written;
            if (to_copy > available) to_copy = available;
            memcpy(dst_interleaved + samples_written,
                   m_impl->audio_buf.data() + m_impl->audio_buf_pos,
                   to_copy * sizeof(float));
            m_impl->audio_buf_pos += to_copy;
            samples_written += to_copy;
            if (m_impl->audio_buf_pos >= (int)m_impl->audio_buf.size()) {
                m_impl->audio_buf.clear();
                m_impl->audio_buf_pos = 0;
            }
            continue;
        }

        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        HRESULT hr = m_impl->reader->ReadSample(
            (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,
            0, nullptr, &flags, &timestamp, &sample);

        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
        if (!sample) continue;

        ComPtr<IMFMediaBuffer> buffer;
        sample->ConvertToContiguousBuffer(&buffer);
        if (!buffer) continue;

        BYTE *data = nullptr;
        DWORD len = 0;
        buffer->Lock(&data, nullptr, &len);
        int n_floats = (int)(len / sizeof(float));
        m_impl->audio_buf.resize(n_floats);
        memcpy(m_impl->audio_buf.data(), data, len);
        buffer->Unlock();
        m_impl->audio_buf_pos = 0;
    }

    return samples_written / nb_channels;
}

avb_result AvbBackendMediaFoundation::read_video_frame(avb_video_frame &out_frame) {
    if (!m_impl->reader || m_impl->video_stream_idx < 0) return AVB_ERROR_STREAM_NOT_FOUND;

    DWORD flags = 0;
    LONGLONG timestamp = 0;
    ComPtr<IMFSample> sample;
    HRESULT hr = m_impl->reader->ReadSample(
        (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
        0, nullptr, &flags, &timestamp, &sample);

    if (FAILED(hr)) return AVB_ERROR_DECODE_FAILED;
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return AVB_ERROR_EOF;
    if (!sample) return AVB_ERROR_DECODE_FAILED;

    ComPtr<IMFMediaBuffer> buffer;
    sample->ConvertToContiguousBuffer(&buffer);
    if (!buffer) return AVB_ERROR_DECODE_FAILED;

    BYTE *data = nullptr;
    DWORD len = 0;
    buffer->Lock(&data, nullptr, &len);
    m_impl->video_frame_buf.resize(len);
    memcpy(m_impl->video_frame_buf.data(), data, len);
    buffer->Unlock();

    out_frame.width     = m_impl->width;
    out_frame.height    = m_impl->height;
    out_frame.format    = AVB_PIXEL_FORMAT_BGRA8;
    out_frame.stride    = m_impl->width * 4;
    out_frame.pts_sec   = (double)timestamp / 1e7;
    out_frame.data      = m_impl->video_frame_buf.data();
    out_frame.data_size = (int)len;

    return AVB_OK;
}

void AvbBackendMediaFoundation::release_video_frame(avb_video_frame &frame) {
    memset(&frame, 0, sizeof(frame));
}

#else // !_WIN32

AvbBackendMediaFoundation::AvbBackendMediaFoundation() {
    m_last_error = "Media Foundation backend is only available on Windows.";
}
AvbBackendMediaFoundation::~AvbBackendMediaFoundation() {}
const char *AvbBackendMediaFoundation::get_backend_name() const { return "mediafoundation"; }
const char *AvbBackendMediaFoundation::get_last_error() const {
    return m_last_error.empty() ? nullptr : m_last_error.c_str();
}
avb_result AvbBackendMediaFoundation::open_file(const char *, const avb_open_options &) {
    return AVB_ERROR_BACKEND_NOT_AVAILABLE;
}
avb_result AvbBackendMediaFoundation::get_media_info(avb_media_info &) {
    return AVB_ERROR_BACKEND_NOT_AVAILABLE;
}
avb_result AvbBackendMediaFoundation::seek(double) { return AVB_ERROR_BACKEND_NOT_AVAILABLE; }
int AvbBackendMediaFoundation::read_audio_f32(float *, int) { return 0; }
avb_result AvbBackendMediaFoundation::read_video_frame(avb_video_frame &) {
    return AVB_ERROR_BACKEND_NOT_AVAILABLE;
}
void AvbBackendMediaFoundation::release_video_frame(avb_video_frame &) {}

#endif // _WIN32
