#include "backends/gstreamer/avb_runtime_gstreamer.hpp"
#include "avb_media_rules.hpp"

#if defined(AVB_ENABLE_GSTREAMER)

#include "backends/gstreamer/avb_gstreamer_loader.hpp"

using avb::detail::Container;
using avb::detail::audio_only_container;
using avb::capability::add_audio_codec;
using avb::capability::add_device;
using avb::capability::add_memory;
using avb::capability::add_software_pixel_formats;
using avb::capability::add_video_codec;

namespace {

bool has_element(const AvbGstFuncs &gst, const char *name) {
    GstElement *element = gst.gst_element_factory_make(name, nullptr);
    if (!element) return false;
    gst.gst_object_unref(element);
    return true;
}

bool has_any(const AvbGstFuncs &gst, const char *const *names) {
    for (int i = 0; names[i]; ++i) {
        if (has_element(gst, names[i])) return true;
    }
    return false;
}

bool has_vaapi(const AvbGstFuncs &gst) {
    static const char *const elements[] = {
        "vapostproc", "vah264enc", "vah265enc", "vah264dec", "vah265dec",
        "vavp9lpenc", "vavp9dec", nullptr
    };
    return has_any(gst, elements);
}

bool has_video_decoder(const AvbGstFuncs &gst, avb_video_codec codec) {
    static const char *const h264[] = {
        "avdec_h264", "openh264dec", "vah264dec", nullptr};
    static const char *const hevc[] = {
        "avdec_h265", "vah265dec", "libde265dec", nullptr};
    static const char *const vp8[] = {"vp8dec", "avdec_vp8", nullptr};
    static const char *const vp9[] = {
        "vp9dec", "avdec_vp9", "vavp9dec", nullptr};
    static const char *const av1[] = {
        "av1dec", "dav1ddec", "avdec_av1", nullptr};

    switch (codec) {
        case AVB_VIDEO_CODEC_H264: return has_any(gst, h264);
        case AVB_VIDEO_CODEC_HEVC: return has_any(gst, hevc);
        case AVB_VIDEO_CODEC_VP8:  return has_any(gst, vp8);
        case AVB_VIDEO_CODEC_VP9:  return has_any(gst, vp9);
        case AVB_VIDEO_CODEC_AV1:  return has_any(gst, av1);
        default: return false;
    }
}

bool has_video_encoder(const AvbGstFuncs &gst, avb_video_codec codec) {
    static const char *const h264[] = {
        "x264enc", "openh264enc", "vah264enc", nullptr};
    static const char *const hevc[] = {"x265enc", "vah265enc", nullptr};
    static const char *const vp8[] = {"vp8enc", nullptr};
    static const char *const vp9[] = {"vp9enc", "vavp9lpenc", nullptr};
    static const char *const av1[] = {"av1enc", nullptr};

    switch (codec) {
        case AVB_VIDEO_CODEC_H264: return has_any(gst, h264);
        case AVB_VIDEO_CODEC_HEVC: return has_any(gst, hevc);
        case AVB_VIDEO_CODEC_VP8:  return has_any(gst, vp8);
        case AVB_VIDEO_CODEC_VP9:  return has_any(gst, vp9);
        case AVB_VIDEO_CODEC_AV1:  return has_any(gst, av1);
        default: return false;
    }
}

bool has_audio_decoder(const AvbGstFuncs &gst, avb_audio_codec codec) {
    static const char *const aac[] = {"avdec_aac", "faad", nullptr};
    static const char *const opus[] = {"opusdec", nullptr};
    static const char *const mp3[] = {
        "mpg123audiodec", "avdec_mp3float", "avdec_mp3", nullptr};
    static const char *const flac[] = {"flacdec", nullptr};
    static const char *const vorbis[] = {"vorbisdec", nullptr};

    switch (codec) {
        case AVB_AUDIO_CODEC_AAC:     return has_any(gst, aac);
        case AVB_AUDIO_CODEC_OPUS:    return has_any(gst, opus);
        case AVB_AUDIO_CODEC_MP3:     return has_any(gst, mp3);
        case AVB_AUDIO_CODEC_FLAC:    return has_any(gst, flac);
        case AVB_AUDIO_CODEC_VORBIS:  return has_any(gst, vorbis);
        case AVB_AUDIO_CODEC_PCM_S16:
        case AVB_AUDIO_CODEC_PCM_F32: return has_element(gst, "audioconvert");
        default: return false;
    }
}

bool has_audio_encoder(
    const AvbGstFuncs &gst,
    avb_audio_codec codec,
    Container container) {
    switch (codec) {
        case AVB_AUDIO_CODEC_AAC:
            return has_element(gst, "avenc_aac");
        case AVB_AUDIO_CODEC_OPUS:
            return has_element(gst, "opusenc");
        case AVB_AUDIO_CODEC_MP3:
            return has_element(gst, "lamemp3enc");
        case AVB_AUDIO_CODEC_FLAC:
            return has_element(gst, "flacenc");
        case AVB_AUDIO_CODEC_VORBIS:
            return has_element(gst, "vorbisenc");
        case AVB_AUDIO_CODEC_PCM_S16:
        case AVB_AUDIO_CODEC_PCM_F32:
            return (container == Container::any || container == Container::wav)
                && has_element(gst, "wavenc");
        default:
            return false;
    }
}

bool has_dmabuf_allocator(const AvbGstFuncs &gst) {
    GstAllocator *allocator = gst.gst_dmabuf_allocator_new();
    if (!allocator) return false;
    gst.gst_object_unref(allocator);
    return true;
}

void fill_decoder(
    avb_decoder_capabilities &out,
    Container container,
    const AvbGstFuncs &gst) {
    const avb_video_codec video_codecs[] = {
        AVB_VIDEO_CODEC_H264, AVB_VIDEO_CODEC_HEVC, AVB_VIDEO_CODEC_VP8,
        AVB_VIDEO_CODEC_VP9, AVB_VIDEO_CODEC_AV1
    };
    const avb_audio_codec audio_codecs[] = {
        AVB_AUDIO_CODEC_AAC, AVB_AUDIO_CODEC_OPUS, AVB_AUDIO_CODEC_MP3,
        AVB_AUDIO_CODEC_FLAC, AVB_AUDIO_CODEC_VORBIS,
        AVB_AUDIO_CODEC_PCM_S16, AVB_AUDIO_CODEC_PCM_F32
    };

    if (!audio_only_container(container)) {
        for (avb_video_codec codec : video_codecs) {
            if (has_video_decoder(gst, codec))
                add_video_codec(out, codec, container);
        }
    }
    for (avb_audio_codec codec : audio_codecs) {
        if (has_audio_decoder(gst, codec))
            add_audio_codec(out, codec, container);
    }

    add_software_pixel_formats(out);
    add_memory(out, AVB_VIDEO_MEMORY_CPU);
    add_memory(out, AVB_VIDEO_MEMORY_NATIVE);
    add_device(out, AVB_HW_DEVICE_AUTO);
    if (has_vaapi(gst)) {
        add_device(out, AVB_HW_DEVICE_VAAPI);
        if (has_dmabuf_allocator(gst))
            add_memory(out, AVB_VIDEO_MEMORY_DMABUF);
    }
}

void fill_encoder(
    avb_encoder_capabilities &out,
    Container container,
    const AvbGstFuncs &gst) {
    const avb_video_codec video_codecs[] = {
        AVB_VIDEO_CODEC_H264, AVB_VIDEO_CODEC_HEVC, AVB_VIDEO_CODEC_VP8,
        AVB_VIDEO_CODEC_VP9, AVB_VIDEO_CODEC_AV1
    };
    const avb_audio_codec audio_codecs[] = {
        AVB_AUDIO_CODEC_AAC, AVB_AUDIO_CODEC_OPUS, AVB_AUDIO_CODEC_MP3,
        AVB_AUDIO_CODEC_FLAC, AVB_AUDIO_CODEC_VORBIS,
        AVB_AUDIO_CODEC_PCM_S16, AVB_AUDIO_CODEC_PCM_F32
    };

    if (!audio_only_container(container)) {
        for (avb_video_codec codec : video_codecs) {
            if (has_video_encoder(gst, codec))
                add_video_codec(out, codec, container);
        }
    }
    for (avb_audio_codec codec : audio_codecs) {
        if (has_audio_encoder(gst, codec, container))
            add_audio_codec(out, codec, container);
    }

    add_memory(out, AVB_VIDEO_MEMORY_CPU);
    add_device(out, AVB_HW_DEVICE_AUTO);
    if (has_vaapi(gst)) {
        add_device(out, AVB_HW_DEVICE_VAAPI);
        add_memory(out, AVB_VIDEO_MEMORY_NATIVE);
        if (has_dmabuf_allocator(gst))
            add_memory(out, AVB_VIDEO_MEMORY_DMABUF);
    }
}

}  // namespace

bool avb_probe_gstreamer_decoder(
    avb_decoder_capabilities &out,
    Container container) {
    AvbGstFuncs gst{};
    char error[AVB_MAX_ERROR] = {};
    if (!avb_gst_load(gst, error, sizeof(error))) return false;

    gst.gst_init(nullptr, nullptr);
    fill_decoder(out, container, gst);
    return true;
}

bool avb_probe_gstreamer_encoder(
    avb_encoder_capabilities &out,
    Container container) {
    AvbGstFuncs gst{};
    char error[AVB_MAX_ERROR] = {};
    if (!avb_gst_load(gst, error, sizeof(error))) return false;

    gst.gst_init(nullptr, nullptr);
    fill_encoder(out, container, gst);
    return true;
}

#endif
