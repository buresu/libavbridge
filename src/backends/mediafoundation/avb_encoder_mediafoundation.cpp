#include "avb_encoder_mediafoundation.hpp"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX  // keep <windows.h> from defining min/max macros
#endif

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#include <wrl/client.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

// MF works in 100-ns ("hns") units; convert seconds to that.
static inline LONGLONG sec_to_hns(double s) { return (LONGLONG)std::llround(s * 1e7); }

struct AvbEncoderMediaFoundation::Impl {
    ComPtr<IMFSinkWriter> writer;

    DWORD video_stream = 0;
    DWORD audio_stream = 0;

    bool   has_video = false;
    bool   has_audio = false;

    int              width = 0, height = 0;
    double           frame_rate = 30.0;
    UINT32           fps_num = 30, fps_den = 1;
    avb_pixel_format input_format = AVB_PIXEL_FORMAT_BGRA8;
    bool             video_is_nv12 = false;
    bool             video_is_i420 = false;
    bool             swizzle_rgba   = false; // RGBA8 input -> swizzle to BGRA for RGB32

    int     sample_rate = 0;
    int     channels    = 0;

    long    video_index   = 0; // for derived PTS
    int64_t audio_samples  = 0; // running total for audio PTS

    bool    began    = false;
    bool    finished = false;

    bool    mf_initialized = false;

    std::vector<unsigned char> video_stage; // contiguous repack scratch
    std::vector<int16_t>       audio_stage; // float -> S16 scratch
};

AvbEncoderMediaFoundation::AvbEncoderMediaFoundation() {
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

AvbEncoderMediaFoundation::~AvbEncoderMediaFoundation() {
    if (m_impl) {
        m_impl->writer.Reset();
        if (m_impl->mf_initialized) MFShutdown();
        delete m_impl;
    }
}

const char *AvbEncoderMediaFoundation::get_last_error() const {
    return m_last_error.empty() ? nullptr : m_last_error.c_str();
}

// The Microsoft AAC encoder accepts only a fixed set of average byte rates.
// Snap the requested bits/sec to the nearest supported value.
static UINT32 aac_bytes_per_sec(int bitrate_bps) {
    const UINT32 allowed[] = { 12000, 16000, 20000, 24000 }; // 96/128/160/192 kbps
    UINT32 want = bitrate_bps > 0 ? (UINT32)(bitrate_bps / 8) : 16000;
    UINT32 best = allowed[0];
    UINT32 best_d = (want > best) ? want - best : best - want;
    for (UINT32 v : allowed) {
        UINT32 d = (want > v) ? want - v : v - want;
        if (d < best_d) { best = v; best_d = d; }
    }
    return best;
}

avb_result AvbEncoderMediaFoundation::open(const char *path, const avb_encode_options &options) {
    if (!m_impl->mf_initialized) return AVB_ERROR_BACKEND_NOT_AVAILABLE;
    m_last_error.clear();

    if (!options.video.enable && !options.audio.enable) {
        m_last_error = "Encoder requires at least one of video/audio enabled.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }

    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (wlen <= 0) {
        m_last_error = "Invalid path encoding.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    std::wstring wpath(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath.data(), wlen);

    // Non-realtime: let WriteSample run as fast as it can rather than pacing to
    // the stream's frame rate.
    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 1);
    attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

    HRESULT hr = MFCreateSinkWriterFromURL(wpath.c_str(), nullptr, attrs.Get(), &m_impl->writer);
    if (FAILED(hr)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "MFCreateSinkWriterFromURL failed: 0x%08lx", hr);
        m_last_error = buf;
        return AVB_ERROR_OPEN_FAILED;
    }

    // --- Video ---
    if (options.video.enable) {
        if (options.video.width <= 0 || options.video.height <= 0) {
            m_last_error = "Video width/height must be positive.";
            return AVB_ERROR_INVALID_ARGUMENT;
        }
        m_impl->width      = options.video.width;
        m_impl->height     = options.video.height;
        m_impl->frame_rate = options.video.frame_rate > 0 ? options.video.frame_rate : 30.0;
        MFAverageTimePerFrameToFrameRate(sec_to_hns(1.0 / m_impl->frame_rate),
                                         &m_impl->fps_num, &m_impl->fps_den);
        if (m_impl->fps_den == 0) { m_impl->fps_num = 30; m_impl->fps_den = 1; }

        // Choose the encoder output (compressed) subtype from the requested
        // codec. Media Foundation ships H.264 and (on Win10+, via the HEVC Video
        // Extensions) HEVC encoder MFTs; it has no built-in VP9 encoder.
        GUID     out_subtype;
        UINT32   profile = 0;       // MF_MT_MPEG2_PROFILE value, 0 = leave unset
        const char *vname;
        switch (options.video.codec) {
            case AVB_CODEC_AUTO:
            case AVB_CODEC_H264:
                out_subtype = MFVideoFormat_H264; profile = eAVEncH264VProfile_Main; vname = "H264"; break;
            case AVB_CODEC_HEVC:
                out_subtype = MFVideoFormat_HEVC; profile = eAVEncH265VProfile_Main_420_8; vname = "HEVC"; break;
            case AVB_CODEC_VP9:
                m_last_error = "VP9 encoding is not supported by the Media Foundation "
                               "backend (use the FFmpeg or GStreamer backend).";
                return AVB_ERROR_INVALID_ARGUMENT;
            default:
                m_last_error = "Invalid video codec (use AUTO/H264/HEVC).";
                return AVB_ERROR_INVALID_ARGUMENT;
        }

        // Choose the encoder input subtype from the caller's frame format. NV12
        // and I420 are planar and fed directly; BGRA/RGBA are fed as RGB32. The
        // Sink Writer inserts the color converter to whatever the encoder wants.
        // RGBA is swizzled to BGRA on copy.
        GUID input_subtype;
        if (options.video.input_format == AVB_PIXEL_FORMAT_NV12) {
            m_impl->input_format  = AVB_PIXEL_FORMAT_NV12;
            m_impl->video_is_nv12 = true;
            input_subtype         = MFVideoFormat_NV12;
        } else if (options.video.input_format == AVB_PIXEL_FORMAT_I420) {
            m_impl->input_format  = AVB_PIXEL_FORMAT_I420;
            m_impl->video_is_i420 = true;
            input_subtype         = MFVideoFormat_I420;
        } else if (options.video.input_format == AVB_PIXEL_FORMAT_RGBA8) {
            m_impl->input_format = AVB_PIXEL_FORMAT_RGBA8;
            m_impl->swizzle_rgba = true;
            input_subtype        = MFVideoFormat_RGB32;
        } else {
            m_impl->input_format = AVB_PIXEL_FORMAT_BGRA8;
            input_subtype        = MFVideoFormat_RGB32;
        }

        int bitrate = options.video.bitrate > 0 ? options.video.bitrate : 4000000;

        // Encoded (output) type the stream is muxed as.
        ComPtr<IMFMediaType> out_type;
        MFCreateMediaType(&out_type);
        out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out_type->SetGUID(MF_MT_SUBTYPE, out_subtype);
        out_type->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)bitrate);
        out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (profile) out_type->SetUINT32(MF_MT_MPEG2_PROFILE, profile);
        MFSetAttributeSize(out_type.Get(), MF_MT_FRAME_SIZE, m_impl->width, m_impl->height);
        MFSetAttributeRatio(out_type.Get(), MF_MT_FRAME_RATE, m_impl->fps_num, m_impl->fps_den);
        MFSetAttributeRatio(out_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        hr = m_impl->writer->AddStream(out_type.Get(), &m_impl->video_stream);
        if (FAILED(hr)) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "AddStream (video/%s) failed: 0x%08lx "
                     "(the %s encoder MFT may not be installed)", vname, hr, vname);
            m_last_error = buf;
            return AVB_ERROR_OPEN_FAILED;
        }

        // Uncompressed (input) type the caller feeds.
        ComPtr<IMFMediaType> in_type;
        MFCreateMediaType(&in_type);
        in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in_type->SetGUID(MF_MT_SUBTYPE, input_subtype);
        in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(in_type.Get(), MF_MT_FRAME_SIZE, m_impl->width, m_impl->height);
        MFSetAttributeRatio(in_type.Get(), MF_MT_FRAME_RATE, m_impl->fps_num, m_impl->fps_den);
        MFSetAttributeRatio(in_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        // Positive stride marks top-down rows (our buffers); RGB32 would default
        // to bottom-up otherwise.
        in_type->SetUINT32(MF_MT_DEFAULT_STRIDE,
                           (UINT32)((m_impl->video_is_nv12 || m_impl->video_is_i420)
                                        ? m_impl->width : m_impl->width * 4));

        hr = m_impl->writer->SetInputMediaType(m_impl->video_stream, in_type.Get(), nullptr);
        if (FAILED(hr)) {
            char buf[128];
            snprintf(buf, sizeof(buf), "SetInputMediaType (video) failed: 0x%08lx", hr);
            m_last_error = buf;
            return AVB_ERROR_OPEN_FAILED;
        }
        m_impl->has_video = true;
    }

    // --- Audio ---
    if (options.audio.enable) {
        if (options.audio.sample_rate <= 0 || options.audio.channels <= 0) {
            m_last_error = "Audio sample_rate/channels must be positive.";
            return AVB_ERROR_INVALID_ARGUMENT;
        }
        m_impl->sample_rate = options.audio.sample_rate;
        m_impl->channels    = options.audio.channels;

        // Media Foundation ships an AAC encoder but no Opus encoder.
        switch (options.audio.codec) {
            case AVB_CODEC_AUTO:
            case AVB_CODEC_AAC:
                break;
            case AVB_CODEC_OPUS:
                m_last_error = "Opus encoding is not supported by the Media Foundation "
                               "backend (use the FFmpeg or GStreamer backend).";
                return AVB_ERROR_INVALID_ARGUMENT;
            default:
                m_last_error = "Invalid audio codec (use AUTO/AAC).";
                return AVB_ERROR_INVALID_ARGUMENT;
        }

        // Encoded (output) AAC type.
        ComPtr<IMFMediaType> out_type;
        MFCreateMediaType(&out_type);
        out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        out_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        out_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        out_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)m_impl->sample_rate);
        out_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)m_impl->channels);
        out_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                            aac_bytes_per_sec(options.audio.bitrate));

        hr = m_impl->writer->AddStream(out_type.Get(), &m_impl->audio_stream);
        if (FAILED(hr)) {
            char buf[128];
            snprintf(buf, sizeof(buf), "AddStream (audio/AAC) failed: 0x%08lx", hr);
            m_last_error = buf;
            return AVB_ERROR_OPEN_FAILED;
        }

        // Input type: 16-bit interleaved PCM (we convert the API's float here).
        const UINT32 block_align = (UINT32)m_impl->channels * 2; // 16-bit
        ComPtr<IMFMediaType> in_type;
        MFCreateMediaType(&in_type);
        in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        in_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        in_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        in_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)m_impl->sample_rate);
        in_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)m_impl->channels);
        in_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, block_align);
        in_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                           (UINT32)m_impl->sample_rate * block_align);
        in_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

        hr = m_impl->writer->SetInputMediaType(m_impl->audio_stream, in_type.Get(), nullptr);
        if (FAILED(hr)) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "SetInputMediaType (audio) failed: 0x%08lx "
                     "(AAC requires 44100/48000 Hz, 1/2/6 channels)", hr);
            m_last_error = buf;
            return AVB_ERROR_OPEN_FAILED;
        }
        m_impl->has_audio = true;
    }

    hr = m_impl->writer->BeginWriting();
    if (FAILED(hr)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "BeginWriting failed: 0x%08lx", hr);
        m_last_error = buf;
        return AVB_ERROR_OPEN_FAILED;
    }
    m_impl->began = true;
    return AVB_OK;
}

// Wrap a freshly filled, MF-allocated buffer in a sample with timing and write
// it to the given stream. Takes ownership semantics of `buf` via ComPtr.
static HRESULT write_buffer(IMFSinkWriter *writer, DWORD stream,
                            ComPtr<IMFMediaBuffer> &buf, DWORD length,
                            LONGLONG time_hns, LONGLONG dur_hns) {
    buf->SetCurrentLength(length);
    ComPtr<IMFSample> sample;
    HRESULT hr = MFCreateSample(&sample);
    if (FAILED(hr)) return hr;
    hr = sample->AddBuffer(buf.Get());
    if (FAILED(hr)) return hr;
    sample->SetSampleTime(time_hns);
    sample->SetSampleDuration(dur_hns);
    return writer->WriteSample(stream, sample.Get());
}

avb_result AvbEncoderMediaFoundation::write_video(const avb_video_frame &frame, double pts_sec) {
    if (!m_impl->has_video) return AVB_ERROR_INVALID_ARGUMENT;
    if (frame.format != m_impl->input_format) {
        m_last_error = "Frame pixel format does not match configured input_format.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }

    const int w = m_impl->width;
    const int h = m_impl->height;

    // PTS resolution order: explicit arg, then the frame's own pts (so decoded
    // frames pass straight through), then derived from frame_rate.
    double pts = pts_sec >= 0.0       ? pts_sec
               : frame.pts_sec >= 0.0 ? frame.pts_sec
               : (double)m_impl->video_index / m_impl->frame_rate;
    LONGLONG time_hns = sec_to_hns(pts);
    LONGLONG dur_hns  = sec_to_hns(1.0 / m_impl->frame_rate);

    // Repack the caller's frame into a tightly-packed, top-down buffer matching
    // the declared input stride, then hand it to the Sink Writer.
    DWORD total;
    if (m_impl->video_is_nv12) {
        const int    y_rows = h, c_rows = h / 2, dst_stride = w;
        const size_t y_size = (size_t)dst_stride * y_rows;
        const size_t c_size = (size_t)dst_stride * c_rows;
        total = (DWORD)(y_size + c_size);
        m_impl->video_stage.resize(total);
        unsigned char *dst = m_impl->video_stage.data();
        for (int y = 0; y < y_rows; ++y)
            memcpy(dst + (size_t)y * dst_stride,
                   frame.plane_data[0] + (size_t)y * frame.plane_stride[0], dst_stride);
        for (int y = 0; y < c_rows; ++y)
            memcpy(dst + y_size + (size_t)y * dst_stride,
                   frame.plane_data[1] + (size_t)y * frame.plane_stride[1], dst_stride);
    } else if (m_impl->video_is_i420) {
        // Three tightly-packed planes: Y (w x h), then Cb and Cr (w/2 x h/2).
        const int    cw = w / 2, ch = h / 2;
        const size_t y_size = (size_t)w * h;
        const size_t c_size = (size_t)cw * ch;
        total = (DWORD)(y_size + 2 * c_size);
        m_impl->video_stage.resize(total);
        unsigned char *dst = m_impl->video_stage.data();
        for (int y = 0; y < h; ++y)
            memcpy(dst + (size_t)y * w,
                   frame.plane_data[0] + (size_t)y * frame.plane_stride[0], w);
        for (int y = 0; y < ch; ++y)
            memcpy(dst + y_size + (size_t)y * cw,
                   frame.plane_data[1] + (size_t)y * frame.plane_stride[1], cw);
        for (int y = 0; y < ch; ++y)
            memcpy(dst + y_size + c_size + (size_t)y * cw,
                   frame.plane_data[2] + (size_t)y * frame.plane_stride[2], cw);
    } else {
        const int dst_stride = w * 4;
        total = (DWORD)((size_t)dst_stride * h);
        m_impl->video_stage.resize(total);
        unsigned char *dst = m_impl->video_stage.data();
        for (int y = 0; y < h; ++y) {
            const unsigned char *src = frame.plane_data[0] + (size_t)y * frame.plane_stride[0];
            unsigned char       *row = dst + (size_t)y * dst_stride;
            if (m_impl->swizzle_rgba) {
                for (int x = 0; x < w; ++x) { // RGBA -> BGRA
                    row[x * 4 + 0] = src[x * 4 + 2];
                    row[x * 4 + 1] = src[x * 4 + 1];
                    row[x * 4 + 2] = src[x * 4 + 0];
                    row[x * 4 + 3] = src[x * 4 + 3];
                }
            } else {
                memcpy(row, src, dst_stride);
            }
        }
    }

    ComPtr<IMFMediaBuffer> buf;
    HRESULT hr = MFCreateMemoryBuffer(total, &buf);
    if (FAILED(hr)) { m_last_error = "MFCreateMemoryBuffer (video) failed."; return AVB_ERROR_ENCODE_FAILED; }

    BYTE *data = nullptr;
    if (FAILED(buf->Lock(&data, nullptr, nullptr))) {
        m_last_error = "Lock (video buffer) failed."; return AVB_ERROR_ENCODE_FAILED;
    }
    memcpy(data, m_impl->video_stage.data(), total);
    buf->Unlock();

    hr = write_buffer(m_impl->writer.Get(), m_impl->video_stream, buf, total, time_hns, dur_hns);
    if (FAILED(hr)) {
        char b[128];
        snprintf(b, sizeof(b), "WriteSample (video) failed: 0x%08lx", hr);
        m_last_error = b;
        return AVB_ERROR_ENCODE_FAILED;
    }
    m_impl->video_index++;
    return AVB_OK;
}

avb_result AvbEncoderMediaFoundation::write_audio_f32(const float *src_interleaved, int frames) {
    if (!m_impl->has_audio) return AVB_ERROR_INVALID_ARGUMENT;

    const int    nch    = m_impl->channels;
    const size_t count  = (size_t)frames * nch;
    const DWORD  nbytes = (DWORD)(count * sizeof(int16_t));

    // Convert interleaved float [-1,1] to interleaved 16-bit PCM.
    m_impl->audio_stage.resize(count);
    int16_t *out = m_impl->audio_stage.data();
    for (size_t i = 0; i < count; ++i) {
        float s = src_interleaved[i];
        s = std::max(-1.0f, std::min(1.0f, s));
        out[i] = (int16_t)std::lround(s * 32767.0f);
    }

    ComPtr<IMFMediaBuffer> buf;
    HRESULT hr = MFCreateMemoryBuffer(nbytes, &buf);
    if (FAILED(hr)) { m_last_error = "MFCreateMemoryBuffer (audio) failed."; return AVB_ERROR_ENCODE_FAILED; }

    BYTE *data = nullptr;
    if (FAILED(buf->Lock(&data, nullptr, nullptr))) {
        m_last_error = "Lock (audio buffer) failed."; return AVB_ERROR_ENCODE_FAILED;
    }
    memcpy(data, out, nbytes);
    buf->Unlock();

    LONGLONG time_hns = sec_to_hns((double)m_impl->audio_samples / m_impl->sample_rate);
    LONGLONG dur_hns  = sec_to_hns((double)frames / m_impl->sample_rate);

    hr = write_buffer(m_impl->writer.Get(), m_impl->audio_stream, buf, nbytes, time_hns, dur_hns);
    if (FAILED(hr)) {
        char b[128];
        snprintf(b, sizeof(b), "WriteSample (audio) failed: 0x%08lx", hr);
        m_last_error = b;
        return AVB_ERROR_ENCODE_FAILED;
    }
    m_impl->audio_samples += frames;
    return AVB_OK;
}

avb_result AvbEncoderMediaFoundation::finish() {
    if (!m_impl->writer || !m_impl->began) return AVB_ERROR_INVALID_ARGUMENT;
    if (m_impl->finished) return AVB_OK;

    HRESULT hr = m_impl->writer->Finalize();
    if (FAILED(hr)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Finalize failed: 0x%08lx", hr);
        m_last_error = buf;
        return AVB_ERROR_ENCODE_FAILED;
    }
    m_impl->finished = true;
    return AVB_OK;
}

#else // !_WIN32

AvbEncoderMediaFoundation::AvbEncoderMediaFoundation() { m_impl = nullptr; }
AvbEncoderMediaFoundation::~AvbEncoderMediaFoundation() {}
avb_result AvbEncoderMediaFoundation::open(const char *, const avb_encode_options &) {
    return AVB_ERROR_BACKEND_NOT_AVAILABLE;
}
avb_result AvbEncoderMediaFoundation::write_video(const avb_video_frame &, double) {
    return AVB_ERROR_BACKEND_NOT_AVAILABLE;
}
avb_result AvbEncoderMediaFoundation::write_audio_f32(const float *, int) {
    return AVB_ERROR_BACKEND_NOT_AVAILABLE;
}
avb_result AvbEncoderMediaFoundation::finish() { return AVB_ERROR_BACKEND_NOT_AVAILABLE; }
const char *AvbEncoderMediaFoundation::get_last_error() const {
    return m_last_error.empty() ? nullptr : m_last_error.c_str();
}

#endif // _WIN32
