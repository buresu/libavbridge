#include "backends/mediafoundation/avb_runtime_mediafoundation.hpp"
#include "avb_capability_builder.hpp"

#if defined(AVB_ENABLE_MEDIAFOUNDATION)

#include "backends/mediafoundation/avb_mediafoundation_common.hpp"

#include <codecapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
using avb::detail::Container;
using avb::detail::audio_only_container;
using avb::capability::add_audio_codec;
using avb::capability::add_audio_codec_unchecked;
using avb::capability::add_device;
using avb::capability::add_memory;
using avb::capability::add_software_pixel_formats;
using avb::capability::add_video_codec;

namespace {

bool video_container(Container container) {
    return container == Container::any ||
           container == Container::mp4 ||
           container == Container::mov ||
           container == Container::ivf ||
           container == Container::unknown;
}

GUID subtype_from_wave_tag(uint32_t tag) {
    return GUID{tag, 0x0000, 0x0010,
        {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
}

GUID vorbis_subtype() {
    return GUID{0x8d2fd10b, 0x5841, 0x4a6b,
        {0x89, 0x05, 0x58, 0x8f, 0xec, 0x1a, 0xde, 0xd9}};
}

bool has_mft(
    const GUID &category,
    const GUID &major,
    const GUID &subtype,
    bool encoder) {
    MFT_REGISTER_TYPE_INFO type{major, subtype};
    IMFActivate **activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(
        category, MFT_ENUM_FLAG_ALL,
        encoder ? nullptr : &type,
        encoder ? &type : nullptr,
        &activates, &count);
    if (activates) {
        for (UINT32 i = 0; i < count; ++i) {
            if (activates[i]) activates[i]->Release();
        }
        CoTaskMemFree(activates);
    }
    return SUCCEEDED(hr) && count > 0;
}

int video_subtypes(avb_video_codec codec, GUID *out, int capacity) {
    if (!out || capacity <= 0) return 0;
    switch (codec) {
        case AVB_VIDEO_CODEC_H264:
            out[0] = MFVideoFormat_H264;
            return 1;
        case AVB_VIDEO_CODEC_HEVC:
            if (capacity < 2) return 0;
            out[0] = MFVideoFormat_HEVC;
            out[1] = mf_video_subtype_from_fourcc(mf_fourcc("H265"));
            return 2;
        case AVB_VIDEO_CODEC_VP8:
            out[0] = MFVideoFormat_VP80;
            return 1;
        case AVB_VIDEO_CODEC_VP9:
            out[0] = mf_video_subtype_from_fourcc(mf_fourcc("VP90"));
            return 1;
        case AVB_VIDEO_CODEC_AV1:
            out[0] = MFVideoFormat_AV1;
            return 1;
        default:
            return 0;
    }
}

bool has_video_transform(avb_video_codec codec, bool encoder) {
    GUID subtypes[2] = {};
    int count = video_subtypes(codec, subtypes, 2);
    for (int i = 0; i < count; ++i) {
        if (has_mft(
                encoder ? MFT_CATEGORY_VIDEO_ENCODER
                        : MFT_CATEGORY_VIDEO_DECODER,
                MFMediaType_Video, subtypes[i], encoder)) {
            return true;
        }
    }
    return false;
}

bool can_configure_video_encoder(avb_video_codec codec) {
    GUID subtypes[2] = {};
    int subtype_count = video_subtypes(codec, subtypes, 2);
    if (subtype_count <= 0) return false;

    MFT_REGISTER_TYPE_INFO type{MFMediaType_Video, subtypes[0]};
    IMFActivate **activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_ALL,
        nullptr, &type, &activates, &count);
    if (FAILED(hr) || count == 0) {
        if (activates) CoTaskMemFree(activates);
        return false;
    }

    bool configured = false;
    for (UINT32 i = 0; i < count && !configured; ++i) {
        ComPtr<IMFTransform> encoder;
        if (!activates[i] ||
            FAILED(activates[i]->ActivateObject(IID_PPV_ARGS(&encoder))) ||
            !encoder) {
            continue;
        }

        ComPtr<IMFAttributes> attributes;
        UINT32 is_async = FALSE;
        if (SUCCEEDED(encoder->GetAttributes(&attributes)) && attributes)
            attributes->GetUINT32(MF_TRANSFORM_ASYNC, &is_async);
        if (is_async &&
            (!attributes ||
             FAILED(attributes->SetUINT32(
                 MF_TRANSFORM_ASYNC_UNLOCK, TRUE)))) {
            activates[i]->ShutdownObject();
            continue;
        }

        ComPtr<IMFMediaType> output;
        MFCreateMediaType(&output);
        output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        output->SetGUID(MF_MT_SUBTYPE, subtypes[0]);
        output->SetUINT32(MF_MT_AVG_BITRATE, 1000000);
        output->SetUINT32(
            MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (codec == AVB_VIDEO_CODEC_AV1)
            output->SetUINT32(
                MF_MT_MPEG2_PROFILE, eAVEncAV1VProfile_Main_420_8);
        MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, 320, 240);
        MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, 30, 1);
        MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        ComPtr<IMFMediaType> input;
        MFCreateMediaType(&input);
        input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        input->SetUINT32(
            MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, 320, 240);
        MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, 30, 1);
        MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        input->SetUINT32(MF_MT_DEFAULT_STRIDE, 320);

        HRESULT output_hr = encoder->SetOutputType(0, output.Get(), 0);
        HRESULT input_hr = SUCCEEDED(output_hr)
            ? encoder->SetInputType(0, input.Get(), 0)
            : E_FAIL;
        if (FAILED(output_hr)) {
            HRESULT first_input = encoder->SetInputType(0, input.Get(), 0);
            if (SUCCEEDED(first_input)) {
                output_hr = encoder->SetOutputType(0, output.Get(), 0);
                input_hr = first_input;
            }
        }
        configured = SUCCEEDED(output_hr) && SUCCEEDED(input_hr);
    }

    for (UINT32 i = 0; i < count; ++i) {
        if (activates[i]) activates[i]->Release();
    }
    CoTaskMemFree(activates);
    return configured;
}

bool has_audio_decoder(avb_audio_codec codec) {
    switch (codec) {
        case AVB_AUDIO_CODEC_AAC:
            return has_mft(
                MFT_CATEGORY_AUDIO_DECODER, MFMediaType_Audio,
                MFAudioFormat_AAC, false);
        case AVB_AUDIO_CODEC_MP3:
            return has_mft(
                MFT_CATEGORY_AUDIO_DECODER, MFMediaType_Audio,
                MFAudioFormat_MP3, false);
        case AVB_AUDIO_CODEC_OPUS:
            return has_mft(
                MFT_CATEGORY_AUDIO_DECODER, MFMediaType_Audio,
                subtype_from_wave_tag(0x704f), false);
        case AVB_AUDIO_CODEC_FLAC:
            return has_mft(
                MFT_CATEGORY_AUDIO_DECODER, MFMediaType_Audio,
                subtype_from_wave_tag(0xf1ac), false);
        case AVB_AUDIO_CODEC_VORBIS:
            return has_mft(
                MFT_CATEGORY_AUDIO_DECODER, MFMediaType_Audio,
                vorbis_subtype(), false);
        case AVB_AUDIO_CODEC_PCM_S16:
            return has_mft(
                MFT_CATEGORY_AUDIO_DECODER, MFMediaType_Audio,
                MFAudioFormat_PCM, false);
        case AVB_AUDIO_CODEC_PCM_F32:
            return has_mft(
                MFT_CATEGORY_AUDIO_DECODER, MFMediaType_Audio,
                MFAudioFormat_Float, false);
        default:
            return false;
    }
}

bool has_audio_encoder(avb_audio_codec codec) {
    switch (codec) {
        case AVB_AUDIO_CODEC_AAC:
            return has_mft(
                MFT_CATEGORY_AUDIO_ENCODER, MFMediaType_Audio,
                MFAudioFormat_AAC, true);
        case AVB_AUDIO_CODEC_MP3:
            return has_mft(
                MFT_CATEGORY_AUDIO_ENCODER, MFMediaType_Audio,
                MFAudioFormat_MP3, true);
        case AVB_AUDIO_CODEC_FLAC:
            return has_mft(
                MFT_CATEGORY_AUDIO_ENCODER, MFMediaType_Audio,
                MFAudioFormat_FLAC, true);
        case AVB_AUDIO_CODEC_PCM_S16:
            return has_mft(
                MFT_CATEGORY_AUDIO_ENCODER, MFMediaType_Audio,
                MFAudioFormat_PCM, true);
        case AVB_AUDIO_CODEC_PCM_F32:
            return has_mft(
                MFT_CATEGORY_AUDIO_ENCODER, MFMediaType_Audio,
                MFAudioFormat_Float, true);
        default:
            return false;
    }
}

void fill_decoder(avb_decoder_capabilities &out, Container container) {
    static const avb_video_codec video[] = {
        AVB_VIDEO_CODEC_H264, AVB_VIDEO_CODEC_HEVC,
        AVB_VIDEO_CODEC_VP8, AVB_VIDEO_CODEC_VP9, AVB_VIDEO_CODEC_AV1};
    static const avb_audio_codec audio[] = {
        AVB_AUDIO_CODEC_AAC, AVB_AUDIO_CODEC_OPUS, AVB_AUDIO_CODEC_MP3,
        AVB_AUDIO_CODEC_FLAC, AVB_AUDIO_CODEC_VORBIS,
        AVB_AUDIO_CODEC_PCM_S16, AVB_AUDIO_CODEC_PCM_F32};

    if (!audio_only_container(container)) {
        for (avb_video_codec codec : video) {
            if (has_video_transform(codec, false))
                add_video_codec(out, codec, container);
        }
    }
    if (container != Container::ivf) {
        for (avb_audio_codec codec : audio) {
            if (codec == AVB_AUDIO_CODEC_PCM_S16 ||
                codec == AVB_AUDIO_CODEC_PCM_F32 ||
                has_audio_decoder(codec)) {
                add_audio_codec(out, codec, container);
            }
        }
    }

    add_software_pixel_formats(out);
    add_memory(out, AVB_VIDEO_MEMORY_CPU);
    if (!audio_only_container(container))
        add_memory(out, AVB_VIDEO_MEMORY_NATIVE);
    add_device(out, AVB_HW_DEVICE_AUTO);
    if (!audio_only_container(container))
        add_device(out, AVB_HW_DEVICE_D3D11VA);
}

void fill_encoder(avb_encoder_capabilities &out, Container container) {
    static const avb_video_codec video[] = {
        AVB_VIDEO_CODEC_H264, AVB_VIDEO_CODEC_HEVC,
        AVB_VIDEO_CODEC_VP8, AVB_VIDEO_CODEC_VP9, AVB_VIDEO_CODEC_AV1};

    if (video_container(container)) {
        for (avb_video_codec codec : video) {
            if (codec == AVB_VIDEO_CODEC_AV1 &&
                container != Container::any &&
                container != Container::ivf) {
                continue;
            }
            if (can_configure_video_encoder(codec))
                add_video_codec(out, codec, container);
        }
    }

    switch (container) {
        case Container::any:
            if (has_audio_encoder(AVB_AUDIO_CODEC_AAC))
                add_audio_codec_unchecked(out, AVB_AUDIO_CODEC_AAC);
            if (has_audio_encoder(AVB_AUDIO_CODEC_MP3))
                add_audio_codec_unchecked(out, AVB_AUDIO_CODEC_MP3);
            if (has_audio_encoder(AVB_AUDIO_CODEC_FLAC))
                add_audio_codec_unchecked(out, AVB_AUDIO_CODEC_FLAC);
            add_audio_codec_unchecked(out, AVB_AUDIO_CODEC_PCM_S16);
            add_audio_codec_unchecked(out, AVB_AUDIO_CODEC_PCM_F32);
            break;
        case Container::mp4:
        case Container::mov:
        case Container::m4a:
        case Container::unknown:
            if (has_audio_encoder(AVB_AUDIO_CODEC_AAC))
                add_audio_codec_unchecked(out, AVB_AUDIO_CODEC_AAC);
            break;
        case Container::mp3:
            if (has_audio_encoder(AVB_AUDIO_CODEC_MP3))
                add_audio_codec_unchecked(out, AVB_AUDIO_CODEC_MP3);
            break;
        case Container::flac:
            if (has_audio_encoder(AVB_AUDIO_CODEC_FLAC))
                add_audio_codec_unchecked(out, AVB_AUDIO_CODEC_FLAC);
            break;
        case Container::wav:
            add_audio_codec_unchecked(out, AVB_AUDIO_CODEC_PCM_S16);
            add_audio_codec_unchecked(out, AVB_AUDIO_CODEC_PCM_F32);
            break;
        default:
            break;
    }

    add_memory(out, AVB_VIDEO_MEMORY_CPU);
    if (container == Container::any ||
        container == Container::ivf ||
        container == Container::mp4 ||
        container == Container::mov) {
        add_memory(out, AVB_VIDEO_MEMORY_NATIVE);
    }
    add_device(out, AVB_HW_DEVICE_AUTO);
    if (container == Container::any ||
        container == Container::ivf ||
        container == Container::mp4 ||
        container == Container::mov) {
        add_device(out, AVB_HW_DEVICE_D3D11VA);
    }
}

} // namespace

bool avb_probe_mediafoundation_decoder(
    avb_decoder_capabilities &out,
    Container container) {
    MfStartupScope mf;
    if (!mf.started()) return false;
    fill_decoder(out, container);
    return true;
}

bool avb_probe_mediafoundation_encoder(
    avb_encoder_capabilities &out,
    Container container) {
    MfStartupScope mf;
    if (!mf.started()) return false;
    fill_encoder(out, container);
    return true;
}

#endif
