#include "avb_backend_mediafoundation.hpp"
#include "../../avb_video_codec_registry.hpp"

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
    int audio_track_count = 0;   // selectable audio tracks in the container
    int width        = 0;
    int height       = 0;
    int video_stride = 0;        // bytes per row; may exceed width*4 due to alignment
    bool video_bottom_up = false; // true when MF_MT_DEFAULT_STRIDE is negative

    avb_pixel_format video_avb_fmt = AVB_PIXEL_FORMAT_BGRA8;
    bool swizzle_rgba = false;    // request ARGB32 (BGRA), emit RGBA
    bool video_is_nv12 = false;   // request NV12, emit two planes (Y + CbCr)
    bool video_is_i420 = false;   // request I420, emit three planes (Y + Cb + Cr)

    double duration_sec = 0.0;
    double frame_rate   = 0.0;

    std::string audio_codec_name;
    std::string video_codec_name;

    std::vector<float>         audio_buf;
    int                        audio_buf_pos = 0;
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

    void close_streams() {
        if (custom_video_decoder && custom_video_decoder->close && custom_video_ctx)
            custom_video_decoder->close(custom_video_ctx);
        custom_video_decoder = nullptr;
        custom_video_ctx = nullptr;
        custom_video = false;
        reader.Reset();
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
        audio_buf.clear();
        audio_buf_pos = 0;
        video_frame_buf.clear();
        custom_packet_buf.clear();
        seek_target_sec    = 0.0;
        video_seek_pending = false;
        audio_seek_pending = false;
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

static void find_stream_indices(IMFSourceReader *reader, int *audio_idx, int *video_idx,
                                int *audio_count) {
    *audio_idx = -1;
    *video_idx = -1;
    *audio_count = 0;
    for (DWORD i = 0; ; ++i) {
        ComPtr<IMFMediaType> type;
        if (FAILED(reader->GetNativeMediaType(i, 0, &type))) break;

        GUID major = GUID_NULL;
        type->GetGUID(MF_MT_MAJOR_TYPE, &major);

        if (IsEqualGUID(major, MFMediaType_Audio)) {
            if (*audio_idx < 0) *audio_idx = (int)i;
            ++*audio_count;
        }
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

static uint32_t mf_fourcc_from_subtype(const GUID &sub) {
    return sub.Data1;
}

static bool mf_get_blob(IMFMediaType *type, REFGUID key,
                        std::vector<unsigned char> &out) {
    UINT32 size = 0;
    if (!type || FAILED(type->GetBlobSize(key, &size)) || size == 0) return false;
    out.resize(size);
    UINT32 got = 0;
    if (FAILED(type->GetBlob(key, out.data(), size, &got))) {
        out.clear();
        return false;
    }
    out.resize(got);
    return true;
}

static avb_result mf_open_custom_video_decoder(
    IMFSourceReader *reader,
    DWORD stream_idx,
    const avb_decode_options &options,
    const avb_video_decoder_plugin **out_plugin,
    void **out_ctx,
    std::string &out_codec_name,
    int *out_width,
    int *out_height,
    double *out_frame_rate
) {
    if (!options.enable_custom_video_decoders) return AVB_ERROR_STREAM_NOT_FOUND;

    ComPtr<IMFMediaType> native;
    if (FAILED(reader->GetNativeMediaType(stream_idx, 0, &native)) || !native)
        return AVB_ERROR_STREAM_NOT_FOUND;

    GUID sub = GUID_NULL;
    native->GetGUID(MF_MT_SUBTYPE, &sub);

    UINT32 w = 0, h = 0;
    MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &w, &h);
    UINT32 fps_num = 0, fps_den = 1;
    MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &fps_num, &fps_den);

    std::vector<unsigned char> extradata;
    mf_get_blob(native.Get(), MF_MT_MPEG_SEQUENCE_HEADER, extradata);

    avb_video_stream_info stream{};
    stream.stream_index = (int)stream_idx;
    stream.width = (int)w;
    stream.height = (int)h;
    stream.frame_rate = fps_den != 0 ? (double)fps_num / fps_den : 0.0;
    stream.codec_tag = mf_fourcc_from_subtype(sub);
    stream.extradata = extradata.empty() ? nullptr : extradata.data();
    stream.extradata_size = (int)extradata.size();
    stream.time_base_num = 1;
    stream.time_base_den = 10000000;

    out_codec_name = mf_subtype_name(sub);
    stream.codec_name = out_codec_name.empty() ? nullptr : out_codec_name.c_str();

    const avb_video_decoder_plugin *plugin =
        avb_find_video_decoder_plugin(stream, options);
    if (!plugin) return AVB_ERROR_STREAM_NOT_FOUND;

    void *ctx = nullptr;
    avb_result res = plugin->open(&ctx, &stream, &options);
    if (res != AVB_OK) return res;

    HRESULT hr = reader->SetCurrentMediaType(stream_idx, nullptr, native.Get());
    if (FAILED(hr)) {
        if (plugin->close && ctx) plugin->close(ctx);
        return AVB_ERROR_OPEN_FAILED;
    }

    *out_plugin = plugin;
    *out_ctx = ctx;
    if (out_width) *out_width = (int)w;
    if (out_height) *out_height = (int)h;
    if (out_frame_rate) *out_frame_rate = stream.frame_rate;
    return AVB_OK;
}

avb_result AvbBackendMediaFoundation::open_file(const char *path, const avb_decode_options &options) {
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

    int found_audio = -1, found_video = -1, audio_count = 0;
    find_stream_indices(m_impl->reader.Get(), &found_audio, &found_video, &audio_count);
    m_impl->audio_track_count = audio_count;

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
        avb_result custom_res = mf_open_custom_video_decoder(
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
        // Pick the output subtype the Video Processor MFT should convert to.
        // NV12 is emitted as two planes (Y + interleaved CbCr); ARGB32 is a
        // packed 32-bit BGRA buffer, optionally swizzled to RGBA after the copy.
        GUID want_subtype;
        if (options.video_format == AVB_PIXEL_FORMAT_NV12) {
            m_impl->video_avb_fmt = AVB_PIXEL_FORMAT_NV12;
            m_impl->video_is_nv12 = true;
            m_impl->swizzle_rgba  = false;
            want_subtype          = MFVideoFormat_NV12;
        } else if (options.video_format == AVB_PIXEL_FORMAT_I420) {
            // I420 is emitted as three planes (Y + Cb + Cr at half size).
            m_impl->video_avb_fmt = AVB_PIXEL_FORMAT_I420;
            m_impl->video_is_i420 = true;
            m_impl->swizzle_rgba  = false;
            want_subtype          = MFVideoFormat_I420;
        } else {
            m_impl->video_avb_fmt = (options.video_format == AVB_PIXEL_FORMAT_RGBA8)
                ? AVB_PIXEL_FORMAT_RGBA8 : AVB_PIXEL_FORMAT_BGRA8;
            m_impl->swizzle_rgba = (options.video_format == AVB_PIXEL_FORMAT_RGBA8);
            // MFVideoFormat_ARGB32 = D3DFMT_A8R8G8B8; memory layout on LE is BGRA.
            // RGBA8 is produced by swizzling this BGRA output after the copy.
            want_subtype = MFVideoFormat_ARGB32;
        }

        ComPtr<IMFMediaType> want;
        MFCreateMediaType(&want);
        want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        want->SetGUID(MF_MT_SUBTYPE, want_subtype);

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
    if (m_impl->custom_video_decoder && m_impl->custom_video_decoder->flush)
        m_impl->custom_video_decoder->flush(m_impl->custom_video_ctx);

    // Arm pre-roll dropping for whichever streams are active.
    m_impl->seek_target_sec    = seconds;
    m_impl->video_seek_pending = (m_impl->video_stream_idx >= 0);
    m_impl->audio_seek_pending = (m_impl->audio_stream_idx >= 0);
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

        // Drop pre-roll samples that precede a pending seek target.
        if (m_impl->audio_seek_pending) {
            if ((double)ts / 1e7 + 1e-6 < m_impl->seek_target_sec) continue;
            m_impl->audio_seek_pending = false;
        }

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

    DWORD    flags = 0;
    LONGLONG ts    = 0;
    ComPtr<IMFSample> sample;
    for (;;) {
        flags = 0;
        ts    = 0;
        sample.Reset();
        HRESULT hr = m_impl->reader->ReadSample(
            (DWORD)m_impl->video_stream_idx, 0, nullptr, &flags, &ts, &sample);

        if (FAILED(hr)) {
            char buf[128];
            snprintf(buf, sizeof(buf), "ReadSample (video) failed: 0x%08lx", hr);
            m_last_error = buf;
            return AVB_ERROR_DECODE_FAILED;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return AVB_ERROR_EOF;
        if (!sample) {
            // No sample this call (e.g. a stream tick). Keep reading while we are
            // dropping seek pre-roll; otherwise preserve the decode-failed signal.
            if (m_impl->video_seek_pending) continue;
            return AVB_ERROR_DECODE_FAILED;
        }

        // Drop pre-roll frames that precede a pending seek target.
        if (m_impl->video_seek_pending) {
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

            avb_encoded_packet packet{};
            packet.data = m_impl->custom_packet_buf.data();
            packet.size = (int)m_impl->custom_packet_buf.size();
            packet.pts_sec = (double)ts / 1e7;
            packet.duration_sec = dur > 0 ? (double)dur / 1e7 : 0.0;
            packet.keyframe = clean_point ? 1 : 0;
            packet.stream_index = m_impl->video_stream_idx;
            packet.pts = ts;
            packet.dts = ts;
            packet.duration = dur;
            packet.time_base_num = 1;
            packet.time_base_den = 10000000;

            avb_result res = m_impl->custom_video_decoder->decode_packet(
                m_impl->custom_video_ctx, &packet, &out_frame);
            if (res == AVB_ERROR_AGAIN) continue;
            if (res == AVB_OK && out_frame.pts_sec < 0.0)
                out_frame.pts_sec = packet.pts_sec;
            return res;
        }
        break;
    }

    const int w = m_impl->width;
    const int h = m_impl->height;

    // NV12: two planes, Y at full resolution and interleaved CbCr at half
    // height. Both planes are packed tightly (stride == width) into one buffer.
    if (m_impl->video_is_nv12) {
        const int    y_rows = h;
        const int    c_rows = h / 2;
        const int    dst_stride = w;            // width bytes per row for both planes
        const size_t y_size = (size_t)dst_stride * y_rows;
        const size_t c_size = (size_t)dst_stride * c_rows;
        const size_t total  = y_size + c_size;
        m_impl->video_frame_buf.resize(total);
        unsigned char *dst = m_impl->video_frame_buf.data();

        bool copied = false;
        DWORD buf_count = 0;
        sample->GetBufferCount(&buf_count);

        if (buf_count == 1) {
            ComPtr<IMFMediaBuffer> raw;
            sample->GetBufferByIndex(0, &raw);

            ComPtr<IMF2DBuffer> buf2d;
            if (raw && SUCCEEDED(raw.As(&buf2d))) {
                BYTE *scan0 = nullptr;
                LONG  pitch = 0;
                if (SUCCEEDED(buf2d->Lock2D(&scan0, &pitch))) {
                    // For NV12 the CbCr plane follows the Y plane at the same
                    // pitch, starting pitch*height bytes after the first scanline.
                    const BYTE *c_src = scan0 + (ptrdiff_t)pitch * y_rows;
                    for (int y = 0; y < y_rows; ++y)
                        memcpy(dst + (size_t)y * dst_stride,
                               scan0 + (ptrdiff_t)y * pitch, dst_stride);
                    for (int y = 0; y < c_rows; ++y)
                        memcpy(dst + y_size + (size_t)y * dst_stride,
                               c_src + (ptrdiff_t)y * pitch, dst_stride);
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
            if (FAILED(buf->Lock(&data, nullptr, &len))) return AVB_ERROR_DECODE_FAILED;

            // A plain contiguous buffer is laid out with the default stride.
            const int src_stride = (m_impl->video_stride > 0) ? m_impl->video_stride : w;
            const BYTE *c_src = data + (size_t)src_stride * y_rows;
            for (int y = 0; y < y_rows; ++y)
                memcpy(dst + (size_t)y * dst_stride,
                       data + (size_t)y * src_stride, dst_stride);
            for (int y = 0; y < c_rows; ++y)
                memcpy(dst + y_size + (size_t)y * dst_stride,
                       c_src + (size_t)y * src_stride, dst_stride);
            buf->Unlock();
        }

        out_frame = {};
        out_frame.width       = w;
        out_frame.height      = h;
        out_frame.format      = AVB_PIXEL_FORMAT_NV12;
        out_frame.pts_sec     = (double)ts / 1e7;
        out_frame.plane_count = 2;
        out_frame.plane_data[0]   = dst;
        out_frame.plane_stride[0] = dst_stride;
        out_frame.plane_data[1]   = dst + y_size;
        out_frame.plane_stride[1] = dst_stride;
        out_frame.data      = out_frame.plane_data[0];
        out_frame.stride    = out_frame.plane_stride[0];
        out_frame.data_size = (int)total;
        return AVB_OK;
    }

    // I420: three planes, Y at full resolution, Cb and Cr at half resolution.
    // All packed tightly (stride == width / width-half) into one buffer.
    if (m_impl->video_is_i420) {
        const int    cw = w / 2, ch = h / 2;
        const size_t y_size = (size_t)w * h;
        const size_t c_size = (size_t)cw * ch;
        const size_t total  = y_size + 2 * c_size;
        m_impl->video_frame_buf.resize(total);
        unsigned char *dst = m_impl->video_frame_buf.data();
        unsigned char *dst_u = dst + y_size;
        unsigned char *dst_v = dst + y_size + c_size;

        // Copy three planes given the source Y pitch (chroma pitch is half).
        auto copy_planes = [&](const BYTE *y_src, int y_pitch) {
            const int   c_pitch = y_pitch / 2;
            const BYTE *u_src   = y_src + (size_t)y_pitch * h;
            const BYTE *v_src   = u_src + (size_t)c_pitch * ch;
            for (int y = 0; y < h; ++y)
                memcpy(dst + (size_t)y * w, y_src + (ptrdiff_t)y * y_pitch, w);
            for (int y = 0; y < ch; ++y)
                memcpy(dst_u + (size_t)y * cw, u_src + (ptrdiff_t)y * c_pitch, cw);
            for (int y = 0; y < ch; ++y)
                memcpy(dst_v + (size_t)y * cw, v_src + (ptrdiff_t)y * c_pitch, cw);
        };

        bool copied = false;
        DWORD buf_count = 0;
        sample->GetBufferCount(&buf_count);

        if (buf_count == 1) {
            ComPtr<IMFMediaBuffer> raw;
            sample->GetBufferByIndex(0, &raw);

            ComPtr<IMF2DBuffer> buf2d;
            if (raw && SUCCEEDED(raw.As(&buf2d))) {
                BYTE *scan0 = nullptr;
                LONG  pitch = 0;
                if (SUCCEEDED(buf2d->Lock2D(&scan0, &pitch))) {
                    copy_planes(scan0, (int)pitch);
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
            if (FAILED(buf->Lock(&data, nullptr, &len))) return AVB_ERROR_DECODE_FAILED;

            const int src_stride = (m_impl->video_stride > 0) ? m_impl->video_stride : w;
            copy_planes(data, src_stride);
            buf->Unlock();
        }

        out_frame = {};
        out_frame.width       = w;
        out_frame.height      = h;
        out_frame.format      = AVB_PIXEL_FORMAT_I420;
        out_frame.pts_sec     = (double)ts / 1e7;
        out_frame.plane_count = 3;
        out_frame.plane_data[0]   = dst;
        out_frame.plane_stride[0] = w;
        out_frame.plane_data[1]   = dst_u;
        out_frame.plane_stride[1] = cw;
        out_frame.plane_data[2]   = dst_v;
        out_frame.plane_stride[2] = cw;
        out_frame.data      = out_frame.plane_data[0];
        out_frame.stride    = out_frame.plane_stride[0];
        out_frame.data_size = (int)total;
        return AVB_OK;
    }

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
        if (FAILED(buf->Lock(&data, nullptr, &len))) return AVB_ERROR_DECODE_FAILED;

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
    if (m_impl && m_impl->custom_video_decoder) {
        if (m_impl->custom_video_decoder->release_frame)
            m_impl->custom_video_decoder->release_frame(m_impl->custom_video_ctx, &frame);
        memset(&frame, 0, sizeof(frame));
        return;
    }
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
avb_result AvbBackendMediaFoundation::open_file(const char *, const avb_decode_options &) {
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
