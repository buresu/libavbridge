#include "backends/avfoundation/avb_runtime_avfoundation.hpp"
#include "avb_media_rules.hpp"

#if defined(AVB_ENABLE_AVFOUNDATION)

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>

using avb::detail::Container;
using avb::detail::audio_only_container;
using avb::capability::add_audio_codec;
using avb::capability::add_device;
using avb::capability::add_external;
using avb::capability::add_memory;
using avb::capability::add_software_pixel_formats;
using avb::capability::add_video_codec;

namespace {

bool has_audio_codec(UInt32 format_id, bool encoder) {
    if (format_id == 0) return false;

    UInt32 size = 0;
    OSStatus status = AudioFormatGetPropertyInfo(
        encoder ? kAudioFormatProperty_Encoders : kAudioFormatProperty_Decoders,
        sizeof(UInt32), &format_id, &size);
    return status == noErr && size > 0;
}

UInt32 audio_format_id(avb_audio_codec codec) {
    switch (codec) {
        case AVB_AUDIO_CODEC_AAC:  return kAudioFormatMPEG4AAC;
        case AVB_AUDIO_CODEC_MP3:  return kAudioFormatMPEGLayer3;
        case AVB_AUDIO_CODEC_FLAC: return kAudioFormatFLAC;
        case AVB_AUDIO_CODEC_OPUS: return kAudioFormatOpus;
        default:                   return 0;
    }
}

CMVideoCodecType video_codec_type(avb_video_codec codec) {
    switch (codec) {
        case AVB_VIDEO_CODEC_H264: return kCMVideoCodecType_H264;
        case AVB_VIDEO_CODEC_HEVC: return kCMVideoCodecType_HEVC;
        case AVB_VIDEO_CODEC_VP9:  return static_cast<CMVideoCodecType>('vp09');
        case AVB_VIDEO_CODEC_AV1:  return static_cast<CMVideoCodecType>('av01');
        default:                   return 0;
    }
}

bool has_video_encoder(CMVideoCodecType type) {
    if (type == 0) return false;

    CFArrayRef list = nullptr;
    if (VTCopyVideoEncoderList(nullptr, &list) != noErr || !list) return false;

    bool found = false;
    CFIndex count = CFArrayGetCount(list);
    for (CFIndex i = 0; i < count && !found; ++i) {
        auto entry = static_cast<CFDictionaryRef>(
            CFArrayGetValueAtIndex(list, i));
        auto codec_type = static_cast<CFNumberRef>(
            CFDictionaryGetValue(entry, kVTVideoEncoderList_CodecType));
        int32_t value = 0;
        if (codec_type &&
            CFNumberGetValue(codec_type, kCFNumberSInt32Type, &value) &&
            static_cast<CMVideoCodecType>(value) == type) {
            found = true;
        }
    }
    CFRelease(list);
    return found;
}

bool can_decode(avb_video_codec codec) {
    switch (codec) {
        case AVB_VIDEO_CODEC_H264:
        case AVB_VIDEO_CODEC_HEVC:
            return true;
        case AVB_VIDEO_CODEC_VP9:
        case AVB_VIDEO_CODEC_AV1:
            return VTIsHardwareDecodeSupported(video_codec_type(codec));
        default:
            return false;
    }
}

}  // namespace

bool avb_probe_avfoundation_decoder(
    avb_decoder_capabilities &out,
    Container container) {
    static const avb_video_codec video_codecs[] = {
        AVB_VIDEO_CODEC_H264, AVB_VIDEO_CODEC_HEVC,
        AVB_VIDEO_CODEC_VP9, AVB_VIDEO_CODEC_AV1
    };
    static const avb_audio_codec audio_codecs[] = {
        AVB_AUDIO_CODEC_AAC, AVB_AUDIO_CODEC_MP3, AVB_AUDIO_CODEC_FLAC,
        AVB_AUDIO_CODEC_OPUS, AVB_AUDIO_CODEC_PCM_S16, AVB_AUDIO_CODEC_PCM_F32
    };

    if (!audio_only_container(container)) {
        for (avb_video_codec codec : video_codecs) {
            if (can_decode(codec)) add_video_codec(out, codec, container);
        }
    }
    for (avb_audio_codec codec : audio_codecs) {
        if (codec == AVB_AUDIO_CODEC_PCM_S16 ||
            codec == AVB_AUDIO_CODEC_PCM_F32 ||
            has_audio_codec(audio_format_id(codec), false)) {
            add_audio_codec(out, codec, container);
        }
    }

    add_software_pixel_formats(out);
    add_memory(out, AVB_VIDEO_MEMORY_CPU);
    if (!audio_only_container(container))
        add_external(out, AVB_VIDEO_EXTERNAL_CVPIXEL_BUFFER);
    add_device(out, AVB_HW_DEVICE_AUTO);
    add_device(out, AVB_HW_DEVICE_VIDEOTOOLBOX);
    return true;
}

bool avb_probe_avfoundation_encoder(
    avb_encoder_capabilities &out,
    Container container) {
    static const avb_video_codec video_codecs[] = {
        AVB_VIDEO_CODEC_H264, AVB_VIDEO_CODEC_HEVC
    };

    if (!audio_only_container(container)) {
        for (avb_video_codec codec : video_codecs) {
            if (has_video_encoder(video_codec_type(codec)))
                add_video_codec(out, codec, container);
        }
    }
    if (has_audio_codec(kAudioFormatMPEG4AAC, true))
        add_audio_codec(out, AVB_AUDIO_CODEC_AAC, container);

    add_memory(out, AVB_VIDEO_MEMORY_CPU);
    if (!audio_only_container(container))
        add_external(out, AVB_VIDEO_EXTERNAL_CVPIXEL_BUFFER);
    add_device(out, AVB_HW_DEVICE_AUTO);
    add_device(out, AVB_HW_DEVICE_VIDEOTOOLBOX);
    return true;
}

#endif
