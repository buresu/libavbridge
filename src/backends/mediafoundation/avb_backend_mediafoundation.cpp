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
    int video_stride = 0;        // bytes per row; may exceed width*4 due to alignment
    bool video_bottom_up = false; // true when MF_MT_DEFAULT_STRIDE is negative

    avb_pixel_format video_avb_fmt = AVB_PIXEL_FORMAT_BGRA8;
    bool swizzle_rgba = false;    // request ARGB32 (BGRA), emit RGBA

    double duration_sec = 0.0;
    double frame_rate   = 0.0;

    std::string audio_codec_name;
    std::string video_codec_name;

    std::vector<float>         audio_buf;
    int                        audio_buf_pos = 0;
    std::vector<unsigned char> video_frame_buf;

    bool mf_initialized = false;

    void close_streams() {
        reader.Reset();
        audio_stream_idx = video_stream_idx = -1;
        sample_rate = channels = width = height = video_stride = 0;
        video_bottom_up = false;
        video_avb_fmt = AVB_PIXEL_FORMAT_BGRA8;
        swizzle_rgba = false;
        duration_sec = frame_rate = 0.0;
        audio_codec_name.clear();
        video_codec_name.clear();
        audio_buf.clear();
        audio_buf_pos = 0;
        video_frame_buf.clear();
    }
};

AvbBackendMediaFoundation::AvbBackendMediaFoundation() {
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

AvbBackendMediaFoundation::~AvbBackendMediaFoundation() {
    if (m_impl) {
        m_impl->close_streams();
        if (m_impl->mf_initialized) MFShutdown();
        delete m_impl;
    }
}

const char *AvbBackendMediaFoundation::get_backend_name() const { return "mediafoundation"; }
const char *AvbBackendMediaFoundation::get_last_error() const {
    return m_last_error.empty() ? nullptr : m_last_error.c_str();
}

static void find_stream_indices(IMFSourceReader *reader, int *audio_idx, int *video_idx) {
    *audio_idx = -1;
    *video_idx = -1;
    for (DWORD i = 0; ; ++i) {
        ComPtr<IMFMediaType> type;
        if (FAILED(reader->GetNativeMediaType(i, 0, &type))) break;

        GUID major = GUID_NULL;
        type->GetGUID(MF_MT_MAJOR_TYPE, &major);

        if (*audio_idx < 0 && IsEqualGUID(major, MFMediaType_Audio))
            *audio_idx = (int)i;
        if (*video_idx < 0 && IsEqualGUID(major, MFMediaType_Video))
            *video_idx = (int)i;
    }
}

// Map a Media Foundation subtype GUID to the same codec name FFmpeg reports, so
// avb_media_info::codec_name means the *source* codec consistently across
// backends. We compare against the well-defined subtype constants and fall back
// to the printable FourCC encoded in the GUID's Data1 field (which is how MF
// derives most of these subtypes), so unknown/newer codecs still report sanely.
static std::string mf_subtype_name(const GUID &sub) {
    static const struct { const GUID *guid; const char *name; } kMap[] = {
        { &MFVideoFormat_H264,      "h264"       },
        { &MFVideoFormat_HEVC,      "hevc"       },
        { &MFVideoFormat_MPEG2,     "mpeg2video" },
        { &MFVideoFormat_MP4V,      "mpeg4"      },
        { &MFVideoFormat_MJPG,      "mjpeg"      },
        { &MFVideoFormat_WMV3,      "wmv3"       },
        { &MFAudioFormat_AAC,       "aac"        },
        { &MFAudioFormat_MP3,       "mp3"        },
        { &MFAudioFormat_Dolby_AC3, "ac3"        },
        { &MFAudioFormat_PCM,       "pcm"        },
        { &MFAudioFormat_Float,     "pcm_f32"    },
    };
    for (const auto &e : kMap) {
        if (IsEqualGUID(sub, *e.guid)) return e.name;
    }
    // Printable FourCC fallback from the GUID's Data1 (little-endian FourCC).
    DWORD fcc = sub.Data1;
    char c[5] = {
        (char)(fcc & 0xff),         (char)((fcc >> 8) & 0xff),
        (char)((fcc >> 16) & 0xff), (char)((fcc >> 24) & 0xff), 0
    };
    for (int i = 0; i < 4; ++i) {
        if (c[i] < 0x20 || c[i] > 0x7e) c[i] = '?';
    }
    return std::string(c);
}

// Source-codec name of a native stream, read before the output type is overridden.
static std::string mf_native_codec_name(IMFSourceReader *reader, DWORD stream) {
    ComPtr<IMFMediaType> native;
    if (FAILED(reader->GetNativeMediaType(stream, 0, &native)) || !native) return "";
    GUID sub = GUID_NULL;
    if (FAILED(native->GetGUID(MF_MT_SUBTYPE, &sub))) return "";
    return mf_subtype_name(sub);
}

avb_result AvbBackendMediaFoundation::open_file(const char *path, const avb_open_options &options) {
    if (!m_impl->mf_initialized) return AVB_ERROR_BACKEND_NOT_AVAILABLE;

    m_impl->close_streams();
    m_last_error.clear();

    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (wlen <= 0) {
        m_last_error = "Invalid path encoding.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    std::wstring wpath(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath.data(), wlen);

    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 1);
    // Enables the Video Processor MFT so any codec can be converted to the
    // requested output format regardless of what the decoder natively outputs.
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);

    HRESULT hr = MFCreateSourceReaderFromURL(wpath.c_str(), attrs.Get(), &m_impl->reader);
    if (FAILED(hr)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "MFCreateSourceReaderFromURL failed: 0x%08lx", hr);
        m_last_error = buf;
        return AVB_ERROR_OPEN_FAILED;
    }

    int found_audio = -1, found_video = -1;
    find_stream_indices(m_impl->reader.Get(), &found_audio, &found_video);

    if (!options.enable_audio) found_audio = -1;
    if (!options.enable_video) found_video = -1;
    if (options.audio_stream_index >= 0) found_audio = options.audio_stream_index;
    if (options.video_stream_index >= 0) found_video = options.video_stream_index;

    m_impl->reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);

    if (found_audio >= 0) {
        ComPtr<IMFMediaType> want;
        MFCreateMediaType(&want);
        want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        want->SetGUID(MF_MT_SUBTYPE,    MFAudioFormat_Float);
        // Request a target rate/channel count; the Source Reader inserts the
        // audio resampler as needed. 0 leaves the source value. The effective
        // values are read back from the negotiated type below.
        if (options.audio_sample_rate > 0)
            want->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)options.audio_sample_rate);
        if (options.audio_channels > 0)
            want->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)options.audio_channels);

        hr = m_impl->reader->SetCurrentMediaType((DWORD)found_audio, nullptr, want.Get());
        if (SUCCEEDED(hr)) {
            m_impl->reader->SetStreamSelection((DWORD)found_audio, TRUE);
            m_impl->audio_stream_idx = found_audio;

            ComPtr<IMFMediaType> cur;
            m_impl->reader->GetCurrentMediaType((DWORD)found_audio, &cur);
            if (cur) {
                UINT32 sr = 0, ch = 0;
                cur->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
                cur->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
                m_impl->sample_rate = (int)sr;
                m_impl->channels    = (int)ch;
            }
            m_impl->audio_codec_name =
                mf_native_codec_name(m_impl->reader.Get(), (DWORD)found_audio);
        }
    }

    if (found_video >= 0) {
        // NV12 output is not implemented for this backend yet; the copy path
        // below assumes a packed 32-bit format. Fail clearly rather than emit a
        // malformed planar frame.
        if (options.video_format == AVB_PIXEL_FORMAT_NV12) {
            m_last_error = "NV12 output is not supported by the Media Foundation "
                           "backend yet; use BGRA8 or RGBA8.";
            m_impl->close_streams();
            return AVB_ERROR_INVALID_ARGUMENT;
        }
        m_impl->video_avb_fmt = (options.video_format == AVB_PIXEL_FORMAT_RGBA8)
            ? AVB_PIXEL_FORMAT_RGBA8 : AVB_PIXEL_FORMAT_BGRA8;
        m_impl->swizzle_rgba = (options.video_format == AVB_PIXEL_FORMAT_RGBA8);

        ComPtr<IMFMediaType> want;
        MFCreateMediaType(&want);
        want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        // MFVideoFormat_ARGB32 = D3DFMT_A8R8G8B8; memory layout on LE is BGRA.
        // RGBA8 is produced by swizzling this BGRA output after the copy.
        want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);

        hr = m_impl->reader->SetCurrentMediaType((DWORD)found_video, nullptr, want.Get());
        if (SUCCEEDED(hr)) {
            m_impl->reader->SetStreamSelection((DWORD)found_video, TRUE);
            m_impl->video_stream_idx = found_video;

            ComPtr<IMFMediaType> cur;
            m_impl->reader->GetCurrentMediaType((DWORD)found_video, &cur);
            if (cur) {
                UINT32 w = 0, h = 0;
                MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h);
                m_impl->width  = (int)w;
                m_impl->height = (int)h;

                UINT32 num = 0, den = 1;
                MFGetAttributeRatio(cur.Get(), MF_MT_FRAME_RATE, &num, &den);
                if (den != 0) m_impl->frame_rate = (double)num / den;

                // MF_MT_DEFAULT_STRIDE is typed UINT32 but semantically INT32;
                // a negative value indicates bottom-up row order.
                UINT32 stride_raw = 0;
                if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride_raw))) {
                    INT32 s = (INT32)stride_raw;
                    if (s < 0) {
                        m_impl->video_bottom_up = true;
                        m_impl->video_stride    = -s;
                    } else {
                        m_impl->video_stride = (s > 0) ? s : (int)w * 4;
                    }
                } else {
                    m_impl->video_stride = (int)w * 4;
                }
            }
            m_impl->video_codec_name =
                mf_native_codec_name(m_impl->reader.Get(), (DWORD)found_video);
        }
    }

    if (m_impl->audio_stream_idx < 0 && m_impl->video_stream_idx < 0) {
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

avb_result AvbBackendMediaFoundation::get_media_info(avb_media_info &out_info) {
    if (!m_impl->reader) return AVB_ERROR_INVALID_ARGUMENT;

    out_info = {};
    out_info.backend_name = "mediafoundation";
    out_info.duration_sec = m_impl->duration_sec;

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

    m_impl->audio_buf.clear();
    m_impl->audio_buf_pos = 0;
    return AVB_OK;
}

int AvbBackendMediaFoundation::read_audio_f32(float *dst_interleaved, int frames) {
    if (!m_impl->reader || m_impl->audio_stream_idx < 0 || m_impl->channels <= 0) return 0;

    const int nb_ch          = m_impl->channels;
    const int samples_needed = frames * nb_ch;
    int samples_written      = 0;

    while (samples_written < samples_needed) {
        int available = (int)m_impl->audio_buf.size() - m_impl->audio_buf_pos;
        if (available > 0) {
            int to_copy = samples_needed - samples_written;
            if (to_copy > available) to_copy = available;
            memcpy(dst_interleaved + samples_written,
                   m_impl->audio_buf.data() + m_impl->audio_buf_pos,
                   to_copy * sizeof(float));
            m_impl->audio_buf_pos += to_copy;
            samples_written       += to_copy;
            if (m_impl->audio_buf_pos >= (int)m_impl->audio_buf.size()) {
                m_impl->audio_buf.clear();
                m_impl->audio_buf_pos = 0;
            }
            continue;
        }

        DWORD flags = 0;
        LONGLONG ts  = 0;
        ComPtr<IMFSample> sample;
        HRESULT hr = m_impl->reader->ReadSample(
            (DWORD)m_impl->audio_stream_idx, 0, nullptr, &flags, &ts, &sample);

        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
        if (!sample) continue;

        ComPtr<IMFMediaBuffer> buf;
        sample->ConvertToContiguousBuffer(&buf);
        if (!buf) continue;

        BYTE  *data = nullptr;
        DWORD  len  = 0;
        buf->Lock(&data, nullptr, &len);
        int n = (int)(len / sizeof(float));
        m_impl->audio_buf.resize(n);
        memcpy(m_impl->audio_buf.data(), data, n * sizeof(float));
        buf->Unlock();
        m_impl->audio_buf_pos = 0;
    }

    return samples_written / nb_ch;
}

avb_result AvbBackendMediaFoundation::read_video_frame(avb_video_frame &out_frame) {
    if (!m_impl->reader || m_impl->video_stream_idx < 0)
        return AVB_ERROR_STREAM_NOT_FOUND;

    DWORD flags = 0;
    LONGLONG ts  = 0;
    ComPtr<IMFSample> sample;
    HRESULT hr = m_impl->reader->ReadSample(
        (DWORD)m_impl->video_stream_idx, 0, nullptr, &flags, &ts, &sample);

    if (FAILED(hr)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "ReadSample (video) failed: 0x%08lx", hr);
        m_last_error = buf;
        return AVB_ERROR_DECODE_FAILED;
    }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return AVB_ERROR_EOF;
    if (!sample) return AVB_ERROR_DECODE_FAILED;

    const int w         = m_impl->width;
    const int h         = m_impl->height;
    const int row_bytes = w * 4;
    m_impl->video_frame_buf.resize((size_t)row_bytes * h);

    DWORD buf_count = 0;
    sample->GetBufferCount(&buf_count);
    bool copied = false;

    if (buf_count == 1) {
        ComPtr<IMFMediaBuffer> raw;
        sample->GetBufferByIndex(0, &raw);

        ComPtr<IMF2DBuffer> buf2d;
        if (raw && SUCCEEDED(raw.As(&buf2d))) {
            BYTE *scanline0 = nullptr;
            LONG  pitch     = 0;
            if (SUCCEEDED(buf2d->Lock2D(&scanline0, &pitch))) {
                // scanline0 is the top-left visual pixel; pitch may be negative
                // for bottom-up storage, but y*pitch always reaches row y.
                for (int y = 0; y < h; ++y) {
                    memcpy(m_impl->video_frame_buf.data() + (size_t)y * row_bytes,
                           scanline0 + (ptrdiff_t)y * pitch,
                           row_bytes);
                }
                buf2d->Unlock2D();
                copied = true;
            }
        }
    }

    if (!copied) {
        ComPtr<IMFMediaBuffer> buf;
        sample->ConvertToContiguousBuffer(&buf);
        if (!buf) return AVB_ERROR_DECODE_FAILED;

        BYTE  *data = nullptr;
        DWORD  len  = 0;
        hr = buf->Lock(&data, nullptr, &len);
        if (FAILED(hr)) return AVB_ERROR_DECODE_FAILED;

        const int stride = (m_impl->video_stride > 0) ? m_impl->video_stride : row_bytes;
        if (m_impl->video_bottom_up) {
            for (int y = 0; y < h; ++y) {
                memcpy(m_impl->video_frame_buf.data() + (size_t)y * row_bytes,
                       data + (size_t)(h - 1 - y) * stride,
                       row_bytes);
            }
        } else {
            for (int y = 0; y < h; ++y) {
                memcpy(m_impl->video_frame_buf.data() + (size_t)y * row_bytes,
                       data + (size_t)y * stride,
                       row_bytes);
            }
        }
        buf->Unlock();
    }

    // Convert BGRA to RGBA in place if requested.
    if (m_impl->swizzle_rgba) {
        unsigned char *p = m_impl->video_frame_buf.data();
        for (int i = 0; i < w * h; ++i) {
            unsigned char b = p[i * 4 + 0];
            p[i * 4 + 0] = p[i * 4 + 2]; // R
            p[i * 4 + 2] = b;            // B
        }
    }

    out_frame = {};
    out_frame.width       = w;
    out_frame.height      = h;
    out_frame.format      = m_impl->video_avb_fmt;
    out_frame.pts_sec     = (double)ts / 1e7;
    out_frame.plane_count = 1;
    out_frame.plane_data[0]   = m_impl->video_frame_buf.data();
    out_frame.plane_stride[0] = row_bytes;
    out_frame.data      = out_frame.plane_data[0];
    out_frame.stride    = out_frame.plane_stride[0];
    out_frame.data_size = row_bytes * h;

    return AVB_OK;
}

void AvbBackendMediaFoundation::release_video_frame(avb_video_frame &frame) {
    memset(&frame, 0, sizeof(frame));
}

#else // !_WIN32

AvbBackendMediaFoundation::AvbBackendMediaFoundation() {
    m_impl = nullptr;
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
