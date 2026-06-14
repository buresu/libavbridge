#include "avb_decoder_mediafoundation.hpp"
#include "../../avb_video_codec_registry.hpp"

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
    bool ivf_native_output = false;
    bool source_native_output = false;
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
    double                     audio_buf_pts = -1.0;
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
        ivf_native_output = false;
        source_native_output = false;
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
        audio_buf.clear();
        audio_buf_pos = 0;
        audio_buf_pts = -1.0;
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
    static const GUID h264_es = {
        0x3f40f4f0, 0x5622, 0x4ff8,
        { 0xb6, 0xd8, 0xa1, 0x7a, 0x58, 0x4b, 0xee, 0x5e }
    };
    if (IsEqualGUID(sub, h264_es)) return "h264";
    static const GUID vorbis = {
        0x8d2fd10b, 0x5841, 0x4a6b,
        { 0x89, 0x05, 0x58, 0x8f, 0xec, 0x1a, 0xde, 0xd9 }
    };
    if (IsEqualGUID(sub, vorbis)) return "vorbis";

    if (sub.Data2 == 0x0000 && sub.Data3 == 0x0010 &&
        sub.Data4[0] == 0x80 && sub.Data4[1] == 0x00 &&
        sub.Data4[2] == 0x00 && sub.Data4[3] == 0xaa &&
        sub.Data4[4] == 0x00 && sub.Data4[5] == 0x38 &&
        sub.Data4[6] == 0x9b && sub.Data4[7] == 0x71) {
        if (sub.Data1 == 0x704f) return "opus";
        if (sub.Data1 == 0xf1ac) return "flac";
    }

    // Printable FourCC fallback from the GUID's Data1 (little-endian FourCC).
    DWORD fcc = sub.Data1;
    char c[5] = {
        (char)(fcc & 0xff),         (char)((fcc >> 8) & 0xff),
        (char)((fcc >> 16) & 0xff), (char)((fcc >> 24) & 0xff), 0
    };
    if (std::strcmp(c, "H265") == 0 || std::strcmp(c, "HEVS") == 0) return "hevc";
    if (std::strcmp(c, "VP80") == 0) return "vp8";
    if (std::strcmp(c, "VP90") == 0) return "vp9";
    if (std::strcmp(c, "AV01") == 0) return "av1";
    for (int i = 0; i < 4; ++i) {
        if (c[i] < 0x20 || c[i] > 0x7e) c[i] = '?';
    }
    return std::string(c);
}

// Source-codec name of a native stream, read before the output type is overridden.
static std::string mf_native_codec_name(IMFSourceReader *reader, DWORD stream) {
    std::string first;
    for (DWORD i = 0; ; ++i) {
        ComPtr<IMFMediaType> native;
        if (FAILED(reader->GetNativeMediaType(stream, i, &native)) || !native) break;
        GUID sub = GUID_NULL;
        if (FAILED(native->GetGUID(MF_MT_SUBTYPE, &sub))) continue;
        std::string name = mf_subtype_name(sub);
        if (first.empty()) first = name;
        if (name != "pcm" && name != "pcm_f32") return name;
    }
    return first;
}

static uint16_t read_le16(const unsigned char *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_le64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (i * 8);
    return v;
}

static GUID mf_video_subtype_from_fourcc(uint32_t fcc) {
    GUID g = { fcc, 0x0000, 0x0010,
        { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
    return g;
}

static HRESULT get_mft_event_with_timeout(IMFMediaEventGenerator *events,
                                          DWORD timeout_ms,
                                          IMFMediaEvent **out) {
    if (!events || !out) return E_POINTER;
    *out = nullptr;
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        HRESULT hr = events->GetEvent(MF_EVENT_FLAG_NO_WAIT, out);
        if (hr != MF_E_NO_EVENTS_AVAILABLE) return hr;
        if (GetTickCount64() >= deadline) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        Sleep(1);
    }
}

static HRESULT create_video_decoder_mft(const GUID &input_subtype,
                                        IMFTransform **out,
                                        bool *is_async) {
    if (!out || !is_async) return E_POINTER;
    *out = nullptr;
    *is_async = false;

    MFT_REGISTER_TYPE_INFO type{};
    type.guidMajorType = MFMediaType_Video;
    type.guidSubtype = input_subtype;
    IMFActivate **activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_ALL,
                           &type, nullptr, &activates, &count);
    if (SUCCEEDED(hr) && count == 0) hr = MF_E_TOPO_CODEC_NOT_FOUND;
    if (SUCCEEDED(hr)) {
        hr = MF_E_TOPO_CODEC_NOT_FOUND;
        for (UINT32 i = 0; i < count; ++i) {
            ComPtr<IMFTransform> candidate;
            HRESULT activate_hr = activates[i]->ActivateObject(
                IID_PPV_ARGS(&candidate));
            if (FAILED(activate_hr) || !candidate) {
                hr = activate_hr;
                continue;
            }
            ComPtr<IMFAttributes> attributes;
            UINT32 async_flag = FALSE;
            if (SUCCEEDED(candidate->GetAttributes(&attributes)) && attributes)
                attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async_flag);
            if (async_flag &&
                (!attributes ||
                 FAILED(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE)))) {
                activates[i]->ShutdownObject();
                continue;
            }
            hr = candidate.CopyTo(out);
            *is_async = async_flag != FALSE;
            break;
        }
    }
    if (activates) {
        for (UINT32 i = 0; i < count; ++i) {
            if (activates[i]) activates[i]->Release();
        }
        CoTaskMemFree(activates);
    }
    return hr;
}

static HRESULT create_d3d11_device_manager(
    ID3D11Device *input_device,
    ID3D11Device **device, IMFDXGIDeviceManager **manager) {
    if (!device || !manager) return E_POINTER;
    *device = nullptr;
    *manager = nullptr;

    ComPtr<ID3D11Device> created_device = input_device;
    HRESULT hr = S_OK;
    if (!created_device) {
        D3D_FEATURE_LEVEL feature_level{};
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
            D3D11_SDK_VERSION, &created_device, &feature_level, nullptr);
        if (FAILED(hr)) return hr;
    }

    ComPtr<IMFDXGIDeviceManager> created_manager;
    UINT reset_token = 0;
    hr = MFCreateDXGIDeviceManager(&reset_token, &created_manager);
    if (SUCCEEDED(hr))
        hr = created_manager->ResetDevice(created_device.Get(), reset_token);
    if (FAILED(hr)) return hr;

    hr = created_device.CopyTo(device);
    if (SUCCEEDED(hr)) hr = created_manager.CopyTo(manager);
    return hr;
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

avb_result AvbDecoderMediaFoundation::open_ivf(
    const char *path, const avb_decode_options &options) {
    if (!options.enable_video) {
        m_last_error = "IVF input contains video only.";
        return AVB_ERROR_STREAM_NOT_FOUND;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        m_last_error = "Opening IVF input failed.";
        return AVB_ERROR_OPEN_FAILED;
    }
    unsigned char header[32] = {};
    if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
        memcmp(header, "DKIF", 4) != 0 ||
        read_le16(header + 6) < 32) {
        fclose(file);
        m_last_error = "Invalid IVF header.";
        return AVB_ERROR_OPEN_FAILED;
    }

    uint32_t fourcc = read_le32(header + 8);
    const uint32_t vp80 = read_le32((const unsigned char *)"VP80");
    const uint32_t vp90 = read_le32((const unsigned char *)"VP90");
    const uint32_t av01 = read_le32((const unsigned char *)"AV01");
    if (fourcc != vp80 && fourcc != vp90 && fourcc != av01) {
        fclose(file);
        m_last_error = "IVF codec is not VP8, VP9, or AV1.";
        return AVB_ERROR_STREAM_NOT_FOUND;
    }

    m_impl->width = (int)read_le16(header + 12);
    m_impl->height = (int)read_le16(header + 14);
    m_impl->ivf_rate = read_le32(header + 16);
    m_impl->ivf_scale = read_le32(header + 20);
    m_impl->ivf_frame_count = read_le32(header + 24);
    m_impl->ivf_data_offset = read_le16(header + 6);
    if (m_impl->width <= 0 || m_impl->height <= 0 ||
        m_impl->ivf_rate == 0 || m_impl->ivf_scale == 0) {
        fclose(file);
        m_last_error = "Invalid IVF dimensions or time base.";
        return AVB_ERROR_OPEN_FAILED;
    }
    if (m_impl->ivf_data_offset > 32 &&
        fseek(file, m_impl->ivf_data_offset, SEEK_SET) != 0) {
        fclose(file);
        m_last_error = "Seeking to IVF frame data failed.";
        return AVB_ERROR_OPEN_FAILED;
    }

    GUID input_subtype = mf_video_subtype_from_fourcc(fourcc);
    HRESULT hr = create_video_decoder_mft(
        input_subtype, &m_impl->ivf_decoder, &m_impl->ivf_async);
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
        options.video_memory == AVB_VIDEO_MEMORY_NATIVE;
    if (m_impl->ivf_native_output) {
        if (options.video_format != AVB_PIXEL_FORMAT_UNKNOWN &&
            options.video_format != AVB_PIXEL_FORMAT_NV12) {
            fclose(file);
            m_last_error = "Media Foundation native IVF decode requires NV12 output.";
            return AVB_ERROR_INVALID_ARGUMENT;
        }
        hr = create_d3d11_device_manager(
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

    ComPtr<IMFMediaType> input_type;
    MFCreateMediaType(&input_type);
    input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input_type->SetGUID(MF_MT_SUBTYPE, input_subtype);
    input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(input_type.Get(), MF_MT_FRAME_SIZE,
                       m_impl->width, m_impl->height);
    MFSetAttributeRatio(input_type.Get(), MF_MT_FRAME_RATE,
                        m_impl->ivf_rate, m_impl->ivf_scale);
    MFSetAttributeRatio(input_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = m_impl->ivf_decoder->SetInputType(0, input_type.Get(), 0);
    if (FAILED(hr)) {
        fclose(file);
        char buf[160];
        snprintf(buf, sizeof(buf), "SetInputType (IVF decoder) failed: 0x%08lx", hr);
        m_last_error = buf;
        return AVB_ERROR_OPEN_FAILED;
    }

    ComPtr<IMFMediaType> output_type;
    MFCreateMediaType(&output_type);
    output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(output_type.Get(), MF_MT_FRAME_SIZE,
                       m_impl->width, m_impl->height);
    MFSetAttributeRatio(output_type.Get(), MF_MT_FRAME_RATE,
                        m_impl->ivf_rate, m_impl->ivf_scale);
    MFSetAttributeRatio(output_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = m_impl->ivf_decoder->SetOutputType(0, output_type.Get(), 0);
    if (FAILED(hr)) {
        for (DWORD i = 0; ; ++i) {
            ComPtr<IMFMediaType> candidate;
            HRESULT type_hr = m_impl->ivf_decoder->GetOutputAvailableType(
                0, i, &candidate);
            if (type_hr == MF_E_NO_MORE_TYPES) break;
            if (FAILED(type_hr) || !candidate) continue;
            GUID subtype = GUID_NULL;
            candidate->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (!IsEqualGUID(subtype, MFVideoFormat_NV12)) continue;
            MFSetAttributeSize(candidate.Get(), MF_MT_FRAME_SIZE,
                               m_impl->width, m_impl->height);
            MFSetAttributeRatio(candidate.Get(), MF_MT_FRAME_RATE,
                                m_impl->ivf_rate, m_impl->ivf_scale);
            hr = m_impl->ivf_decoder->SetOutputType(0, candidate.Get(), 0);
            if (SUCCEEDED(hr)) break;
        }
    }
    if (FAILED(hr)) {
        fclose(file);
        char buf[160];
        snprintf(buf, sizeof(buf), "SetOutputType (IVF decoder) failed: 0x%08lx", hr);
        m_last_error = buf;
        return AVB_ERROR_OPEN_FAILED;
    }

    MFT_OUTPUT_STREAM_INFO osi{};
    if (SUCCEEDED(m_impl->ivf_decoder->GetOutputStreamInfo(0, &osi))) {
        m_impl->ivf_output_size = osi.cbSize;
        m_impl->ivf_output_flags = osi.dwFlags;
    }
    if (m_impl->ivf_output_size == 0)
        m_impl->ivf_output_size =
            (DWORD)((size_t)m_impl->width * m_impl->height * 3 / 2);

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
    m_impl->video_codec_name =
        fourcc == vp80 ? "vp8" : fourcc == vp90 ? "vp9" : "av1";
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
        options.video_memory == AVB_VIDEO_MEMORY_NATIVE &&
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

    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, native_d3d11 ? 4 : 1);
    // Enables the Video Processor MFT so any codec can be converted to the
    // requested output format regardless of what the decoder natively outputs.
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    if (native_d3d11) {
        HRESULT manager_hr = create_d3d11_device_manager(
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
    find_stream_indices(m_impl->reader.Get(), &found_audio, &found_video, &audio_count);
    m_impl->audio_track_count = audio_count;

    if (!options.enable_audio) found_audio = -1;
    if (!options.enable_video) found_video = -1;
    if (options.audio_stream_index >= 0) found_audio = options.audio_stream_index;
    if (options.video_stream_index >= 0) found_video = options.video_stream_index;

    m_impl->reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);

    if (found_audio >= 0) {
        std::string native_audio_codec =
            mf_native_codec_name(m_impl->reader.Get(), (DWORD)found_audio);

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
            m_impl->audio_codec_name = native_audio_codec;
        }
    }

    if (found_video >= 0) {
        avb_result custom_res = m_impl->source_native_output
            ? AVB_ERROR_STREAM_NOT_FOUND
            : mf_open_custom_video_decoder(
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
            mf_native_codec_name(m_impl->reader.Get(), (DWORD)found_video);

        // Pick the output subtype the Video Processor MFT should convert to.
        // NV12 is emitted as two planes (Y + interleaved CbCr); ARGB32 is a
        // packed 32-bit BGRA buffer, optionally swizzled to RGBA after the copy.
        GUID want_subtype;
        if (options.video_format == AVB_PIXEL_FORMAT_NV12 ||
            (m_impl->source_native_output &&
             options.video_format == AVB_PIXEL_FORMAT_UNKNOWN)) {
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
            m_impl->video_codec_name = native_video_codec;
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

    m_impl->audio_buf.clear();
    m_impl->audio_buf_pos = 0;
    m_impl->audio_buf_pts = -1.0;
    if (m_impl->custom_video_decoder && m_impl->custom_video_decoder->flush)
        m_impl->custom_video_decoder->flush(m_impl->custom_video_ctx);

    // Arm pre-roll dropping for whichever streams are active.
    m_impl->seek_target_sec    = seconds;
    m_impl->video_seek_pending = (m_impl->video_stream_idx >= 0);
    m_impl->audio_seek_pending = (m_impl->audio_stream_idx >= 0);
    return AVB_OK;
}

bool AvbDecoderMediaFoundation::fill_audio_buffer() {
    if (!m_impl || !m_impl->reader || m_impl->audio_stream_idx < 0 || m_impl->channels <= 0)
        return false;

    for (;;) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        HRESULT hr = m_impl->reader->ReadSample(
            (DWORD)m_impl->audio_stream_idx, 0, nullptr, &flags, &ts, &sample);

        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) return false;
        if (!sample) continue;

        // Drop pre-roll samples that precede a pending seek target.
        if (m_impl->audio_seek_pending) {
            if ((double)ts / 1e7 + 1e-6 < m_impl->seek_target_sec) continue;
            m_impl->audio_seek_pending = false;
        }

        ComPtr<IMFMediaBuffer> buf;
        sample->ConvertToContiguousBuffer(&buf);
        if (!buf) continue;

        BYTE *data = nullptr;
        DWORD len = 0;
        buf->Lock(&data, nullptr, &len);
        int n = (int)(len / sizeof(float));
        m_impl->audio_buf.resize(n);
        memcpy(m_impl->audio_buf.data(), data, n * sizeof(float));
        buf->Unlock();
        m_impl->audio_buf_pos = 0;
        m_impl->audio_buf_pts = (double)ts / 1e7;
        return n > 0;
    }
}

static HRESULT update_source_video_type(
    IMFSourceReader *reader, DWORD stream, bool native_output,
    int *width, int *height, int *stride, bool *bottom_up) {
    if (!reader || !width || !height || !stride || !bottom_up)
        return E_POINTER;

    ComPtr<IMFMediaType> current;
    HRESULT hr = reader->GetCurrentMediaType(stream, &current);
    if (FAILED(hr) || !current) return FAILED(hr) ? hr : E_FAIL;

    GUID subtype = GUID_NULL;
    hr = current->GetGUID(MF_MT_SUBTYPE, &subtype);
    if (FAILED(hr)) return hr;
    if (native_output && !IsEqualGUID(subtype, MFVideoFormat_NV12))
        return MF_E_INVALIDMEDIATYPE;

    UINT32 w = 0, h = 0;
    hr = MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &w, &h);
    if (FAILED(hr) || w == 0 || h == 0) return FAILED(hr) ? hr : E_FAIL;
    *width = (int)w;
    *height = (int)h;

    UINT32 stride_raw = 0;
    if (SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride_raw))) {
        INT32 signed_stride = (INT32)stride_raw;
        *bottom_up = signed_stride < 0;
        *stride = signed_stride < 0 ? -signed_stride : signed_stride;
    } else {
        *bottom_up = false;
        *stride = IsEqualGUID(subtype, MFVideoFormat_ARGB32)
            ? (int)w * 4 : (int)w;
    }
    return S_OK;
}

int AvbDecoderMediaFoundation::read_audio_f32(float *dst_interleaved, int frames) {
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
            if (m_impl->audio_buf_pts >= 0.0 && m_impl->sample_rate > 0)
                m_impl->audio_buf_pts +=
                    (double)(to_copy / nb_ch) / m_impl->sample_rate;
            if (m_impl->audio_buf_pos >= (int)m_impl->audio_buf.size()) {
                m_impl->audio_buf.clear();
                m_impl->audio_buf_pos = 0;
                m_impl->audio_buf_pts = -1.0;
            }
            continue;
        }

        if (!fill_audio_buffer()) break;
    }

    return samples_written / nb_ch;
}

double AvbDecoderMediaFoundation::audio_next_pts() {
    if (!m_impl->reader || m_impl->audio_stream_idx < 0 || m_impl->channels <= 0)
        return -1.0;
    if (m_impl->audio_buf_pos >= (int)m_impl->audio_buf.size()) {
        m_impl->audio_buf.clear();
        m_impl->audio_buf_pos = 0;
        m_impl->audio_buf_pts = -1.0;
        if (!fill_audio_buffer()) return -1.0;
    }
    return m_impl->audio_buf_pts;
}

avb_result AvbDecoderMediaFoundation::read_ivf_frame(avb_video_frame &out_frame) {
    if (!m_impl->ivf_file || !m_impl->ivf_decoder)
        return AVB_ERROR_STREAM_NOT_FOUND;

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
            HRESULT type_result = MF_E_INVALIDMEDIATYPE;
            for (DWORD i = 0; ; ++i) {
                ComPtr<IMFMediaType> candidate;
                HRESULT type_hr = m_impl->ivf_decoder->GetOutputAvailableType(
                    0, i, &candidate);
                if (type_hr == MF_E_NO_MORE_TYPES) break;
                if (FAILED(type_hr) || !candidate) continue;
                GUID subtype = GUID_NULL;
                candidate->GetGUID(MF_MT_SUBTYPE, &subtype);
                if (!IsEqualGUID(subtype, MFVideoFormat_NV12)) continue;
                type_result = m_impl->ivf_decoder->SetOutputType(
                    0, candidate.Get(), 0);
                if (SUCCEEDED(type_result)) {
                    UINT32 width = 0, height = 0;
                    if (SUCCEEDED(MFGetAttributeSize(
                            candidate.Get(), MF_MT_FRAME_SIZE,
                            &width, &height)) &&
                        width > 0 && height > 0) {
                        m_impl->width = (int)width;
                        m_impl->height = (int)height;
                    }
                    MFT_OUTPUT_STREAM_INFO osi{};
                    if (SUCCEEDED(m_impl->ivf_decoder->GetOutputStreamInfo(
                            0, &osi))) {
                        m_impl->ivf_output_size = osi.cbSize;
                        m_impl->ivf_output_flags = osi.dwFlags;
                    }
                    break;
                }
            }
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

    ComPtr<IMFSample> decoded;
    for (;;) {
        if (m_impl->ivf_async) {
            ComPtr<IMFMediaEvent> event;
            HRESULT hr = get_mft_event_with_timeout(
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
            if (type == METransformDrainComplete) return AVB_ERROR_EOF;
            if (type != METransformNeedInput) continue;
        } else if (m_impl->ivf_draining) {
            avb_result result = process_output(decoded);
            if (result == AVB_OK) break;
            if (result == AVB_ERROR_AGAIN) return AVB_ERROR_EOF;
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

        unsigned char frame_header[12] = {};
        if (fread(frame_header, 1, sizeof(frame_header), m_impl->ivf_file) !=
            sizeof(frame_header)) {
            m_impl->ivf_eof = true;
            continue;
        }
        uint32_t packet_size = read_le32(frame_header);
        uint64_t timestamp = read_le64(frame_header + 4);
        if (packet_size == 0 || packet_size > 256u * 1024u * 1024u) {
            m_last_error = "Invalid IVF frame size.";
            return AVB_ERROR_DECODE_FAILED;
        }
        m_impl->ivf_packet.resize(packet_size);
        if (fread(m_impl->ivf_packet.data(), 1, packet_size, m_impl->ivf_file) !=
            packet_size) {
            m_last_error = "Truncated IVF frame.";
            return AVB_ERROR_DECODE_FAILED;
        }

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
            return read_ivf_frame(out_frame);
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
        out_frame.memory_type = AVB_VIDEO_MEMORY_NATIVE;
        out_frame.hardware_device = AVB_HW_DEVICE_D3D11VA;
        out_frame.native_handle = texture.Get();
        out_frame.native_handle_id = subresource;
        out_frame.native_owner =
            m_impl->retain_native_frame(decoded.Get(), texture.Get());
        for (int p = 0; p < AVB_MAX_PLANES; ++p) out_frame.dmabuf_fd[p] = -1;
        return AVB_OK;
    }

    const size_t y_size = (size_t)w * h;
    const size_t uv_size = y_size / 2;
    std::vector<unsigned char> nv12(y_size + uv_size);
    bool copied = false;
    DWORD buffer_count = 0;
    decoded->GetBufferCount(&buffer_count);
    if (buffer_count == 1) {
        ComPtr<IMFMediaBuffer> raw;
        decoded->GetBufferByIndex(0, &raw);
        ComPtr<IMF2DBuffer> buffer2d;
        if (raw && SUCCEEDED(raw.As(&buffer2d))) {
            BYTE *scan0 = nullptr;
            LONG pitch = 0;
            if (SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch))) {
                const BYTE *uv = scan0 + (ptrdiff_t)pitch * h;
                for (int y = 0; y < h; ++y)
                    memcpy(nv12.data() + (size_t)y * w,
                           scan0 + (ptrdiff_t)y * pitch, w);
                for (int y = 0; y < h / 2; ++y)
                    memcpy(nv12.data() + y_size + (size_t)y * w,
                           uv + (ptrdiff_t)y * pitch, w);
                buffer2d->Unlock2D();
                copied = true;
            }
        }
    }
    if (!copied) {
        ComPtr<IMFMediaBuffer> contiguous;
        decoded->ConvertToContiguousBuffer(&contiguous);
        BYTE *data = nullptr;
        DWORD length = 0;
        if (!contiguous ||
            FAILED(contiguous->Lock(&data, nullptr, &length)) ||
            length < nv12.size()) {
            if (contiguous && data) contiguous->Unlock();
            m_last_error = "IVF decoder returned an invalid NV12 frame.";
            return AVB_ERROR_DECODE_FAILED;
        }
        memcpy(nv12.data(), data, nv12.size());
        contiguous->Unlock();
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
        out_frame.plane_data[1] = m_impl->video_frame_buf.data() + y_size;
        out_frame.plane_stride[1] = w;
    } else if (m_impl->video_is_i420) {
        const size_t c_size = y_size / 4;
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
        out_frame.plane_data[1] = u;
        out_frame.plane_stride[1] = w / 2;
        out_frame.plane_data[2] = v;
        out_frame.plane_stride[2] = w / 2;
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
                int uu = (int)uv_plane[(size_t)(y / 2) * w + (x & ~1)] - 128;
                int vv = (int)uv_plane[(size_t)(y / 2) * w + (x & ~1) + 1] - 128;
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

        if (FAILED(hr)) {
            char buf[128];
            snprintf(buf, sizeof(buf), "ReadSample (video) failed: 0x%08lx", hr);
            m_last_error = buf;
            return AVB_ERROR_DECODE_FAILED;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return AVB_ERROR_EOF;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            hr = update_source_video_type(
                m_impl->reader.Get(), (DWORD)m_impl->video_stream_idx,
                m_impl->source_native_output, &m_impl->width, &m_impl->height,
                &m_impl->video_stride, &m_impl->video_bottom_up);
            if (FAILED(hr)) {
                char buf[160];
                snprintf(buf, sizeof(buf),
                         "Updating Source Reader video type failed: 0x%08lx",
                         hr);
                m_last_error = buf;
                return AVB_ERROR_DECODE_FAILED;
            }
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
        out_frame.memory_type = AVB_VIDEO_MEMORY_NATIVE;
        out_frame.hardware_device = AVB_HW_DEVICE_D3D11VA;
        out_frame.native_handle = texture.Get();
        out_frame.native_handle_id = subresource;
        out_frame.native_owner =
            m_impl->retain_native_frame(sample.Get(), texture.Get());
        for (int p = 0; p < AVB_MAX_PLANES; ++p) out_frame.dmabuf_fd[p] = -1;
        return AVB_OK;
    }

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
    out_frame.memory_type = AVB_VIDEO_MEMORY_CPU;
    out_frame.hardware_device = AVB_HW_DEVICE_AUTO;
    out_frame.plane_count = 1;
    for (int p = 0; p < AVB_MAX_PLANES; ++p) out_frame.dmabuf_fd[p] = -1;
    out_frame.plane_data[0]   = m_impl->video_frame_buf.data();
    out_frame.plane_stride[0] = row_bytes;
    out_frame.data      = out_frame.plane_data[0];
    out_frame.stride    = out_frame.plane_stride[0];
    out_frame.data_size = row_bytes * h;

    return AVB_OK;
}

void AvbDecoderMediaFoundation::release_video_frame(avb_video_frame &frame) {
    if (m_impl && frame.memory_type == AVB_VIDEO_MEMORY_NATIVE &&
        frame.native_owner) {
        auto lease = m_impl->native_frame_leases.find(frame.native_owner);
        if (lease != m_impl->native_frame_leases.end()) {
            m_impl->native_frame_leases.erase(lease);
            memset(&frame, 0, sizeof(frame));
            return;
        }
    }
    if (m_impl && m_impl->custom_video_decoder) {
        if (m_impl->custom_video_decoder->release_frame)
            m_impl->custom_video_decoder->release_frame(m_impl->custom_video_ctx, &frame);
        memset(&frame, 0, sizeof(frame));
        return;
    }
    memset(&frame, 0, sizeof(frame));
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
avb_result AvbDecoderMediaFoundation::read_video_frame(avb_video_frame &) {
    return AVB_ERROR_BACKEND_NOT_AVAILABLE;
}
void AvbDecoderMediaFoundation::release_video_frame(avb_video_frame &) {}

#endif // _WIN32
