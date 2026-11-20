#include "avb_stream_avfoundation.hh"
#include "avb_bitstream.hpp"
#include "avb_pixel_convert.hpp"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <TargetConditionals.h>
#include <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>

namespace {

constexpr OSStatus k_need_more_input = 'more';

std::string os_message(const char *what, OSStatus status) {
    char buf[200];
    snprintf(buf, sizeof(buf), "%s failed (OSStatus %d)", what, (int)status);
    return buf;
}

class CFScope {
public:
    explicit CFScope(CFTypeRef ref = nullptr) : m_ref(ref) {}
    ~CFScope() { if (m_ref) CFRelease(m_ref); }
    CFScope(const CFScope &) = delete;
    CFScope &operator=(const CFScope &) = delete;
    CFTypeRef get() const { return m_ref; }
private:
    CFTypeRef m_ref;
};

void dict_set_int(CFMutableDictionaryRef dict, CFStringRef key, int32_t value) {
    CFNumberRef number = CFNumberCreate(nullptr, kCFNumberSInt32Type, &value);
    CFDictionarySetValue(dict, key, number);
    CFRelease(number);
}

OSStatus session_set_int(VTSessionRef session, CFStringRef key, int32_t value) {
    CFNumberRef number = CFNumberCreate(nullptr, kCFNumberSInt32Type, &value);
    const OSStatus status = VTSessionSetProperty(session, key, number);
    CFRelease(number);
    return status;
}

CFMutableDictionaryRef new_dict() {
    return CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                     &kCFTypeDictionaryValueCallBacks);
}

int64_t cmtime_ticks(CMTime time, int timescale) {
    if (!CMTIME_IS_VALID(time) || CMTIME_IS_INDEFINITE(time) || time.timescale <= 0) return -1;
    return (int64_t)llroundl((long double)time.value * timescale / time.timescale);
}

OSType cv_pixel_format(avb_pixel_format format) {
    switch (format) {
        case AVB_PIXEL_FORMAT_NV12: return kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        case AVB_PIXEL_FORMAT_I420: return kCVPixelFormatType_420YpCbCr8Planar;
        default: return kCVPixelFormatType_32BGRA;
    }
}

// Copy a validated CPU frame into a locked CVPixelBuffer of the matching
// layout (RGBA is swizzled into the BGRA buffer).
void copy_into_pixel_buffer(const avb_video_frame &frame, CVPixelBufferRef pb) {
    if (CVPixelBufferIsPlanar(pb)) {
        const size_t planes = CVPixelBufferGetPlaneCount(pb);
        for (size_t p = 0; p < planes && (int)p < frame.plane_count; ++p) {
            uint8_t *dst = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pb, p);
            const size_t dst_stride = CVPixelBufferGetBytesPerRowOfPlane(pb, p);
            const size_t rows = CVPixelBufferGetHeightOfPlane(pb, p);
            const size_t row_bytes = std::min(dst_stride, (size_t)frame.plane_stride[p]);
            const size_t width_bytes = p == 0 ? (size_t)frame.width
                : (frame.format == AVB_PIXEL_FORMAT_NV12 ? (size_t)((frame.width + 1) / 2) * 2
                                                          : (size_t)(frame.width + 1) / 2);
            for (size_t y = 0; y < rows; ++y)
                std::memcpy(dst + y * dst_stride, frame.plane_data[p] + y * frame.plane_stride[p],
                            std::min(row_bytes, width_bytes));
        }
        return;
    }
    uint8_t *dst = (uint8_t *)CVPixelBufferGetBaseAddress(pb);
    const size_t dst_stride = CVPixelBufferGetBytesPerRow(pb);
    const bool swap = frame.format == AVB_PIXEL_FORMAT_RGBA8;
    for (int y = 0; y < frame.height; ++y) {
        const uint8_t *src = frame.plane_data[0] + (size_t)y * frame.plane_stride[0];
        uint8_t *row = dst + (size_t)y * dst_stride;
        if (!swap) {
            std::memcpy(row, src, (size_t)frame.width * 4);
            continue;
        }
        for (int x = 0; x < frame.width; ++x) {
            row[x * 4 + 0] = src[x * 4 + 2];
            row[x * 4 + 1] = src[x * 4 + 1];
            row[x * 4 + 2] = src[x * 4 + 0];
            row[x * 4 + 3] = src[x * 4 + 3];
        }
    }
}

// Parameter sets of a VideoToolbox format description, as Annex-B.
std::vector<uint8_t> parameter_sets(CMFormatDescriptionRef format, avb_video_codec codec,
                                    int &nal_length) {
    std::vector<uint8_t> out;
    size_t count = 0;
    int header = 4;
    OSStatus status;
    if (codec == AVB_VIDEO_CODEC_HEVC) {
        if (@available(macOS 10.13, iOS 11.0, *))
            status = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, 0, nullptr, nullptr,
                                                                        &count, &header);
        else
            return out;
    } else {
        status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, nullptr, nullptr,
                                                                    &count, &header);
    }
    if (status != noErr) return out;
    nal_length = header;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t *data = nullptr;
        size_t size = 0;
        if (codec == AVB_VIDEO_CODEC_HEVC) {
            if (@available(macOS 10.13, iOS 11.0, *))
                status = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, i, &data, &size,
                                                                            nullptr, nullptr);
        } else {
            status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, i, &data, &size,
                                                                        nullptr, nullptr);
        }
        if (status == noErr && data && size > 0) avb::detail::append_annexb(out, data, size);
    }
    return out;
}

} // namespace

/* ================================================================ encoder */

struct AvbStreamEncoderAVFoundation::Impl {
    bool is_video = false;
    AvbStreamVideoEncodeConfig video;
    AvbStreamAudioEncodeConfig audio;

    VTCompressionSessionRef session = nullptr;
    std::mutex mutex;
    std::deque<AvbStreamEncodedPacket> queue;
    OSStatus callback_status = noErr;
    std::vector<uint8_t> param_sets; // Annex-B, from the first keyframe's description
    int nal_length = 4;
    bool flushed = false;

    AudioConverterRef converter = nullptr;
    std::vector<float> fifo;
    std::vector<float> handed; // samples given to the converter, valid until the next call
    bool final_chunk = false;
    UInt32 max_packet = 0;
    UInt32 frames_per_packet = 1024;
    int64_t packets_out = 0;
    int leading = 0;
    std::vector<uint8_t> packet_buf;

    ~Impl() {
        if (session) {
            VTCompressionSessionInvalidate(session);
            CFRelease(session);
        }
        if (converter) AudioConverterDispose(converter);
    }

    static void on_output(void *refcon, void *, OSStatus status, VTEncodeInfoFlags flags,
                          CMSampleBufferRef sample) {
        auto *m = static_cast<Impl *>(refcon);
        std::lock_guard<std::mutex> lock(m->mutex);
        if (status != noErr) {
            m->callback_status = status;
            return;
        }
        if (!sample || (flags & kVTEncodeInfo_FrameDropped)) return;
        CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
        if (!block) return;
        AvbStreamEncodedPacket packet;
        packet.data.resize(CMBlockBufferGetDataLength(block));
        if (CMBlockBufferCopyDataBytes(block, 0, packet.data.size(), packet.data.data()) != noErr)
            return;
        packet.pts = cmtime_ticks(CMSampleBufferGetPresentationTimeStamp(sample), AVB_STREAM_VIDEO_TIMESCALE);
        packet.dts = cmtime_ticks(CMSampleBufferGetDecodeTimeStamp(sample), AVB_STREAM_VIDEO_TIMESCALE);
        packet.duration = cmtime_ticks(CMSampleBufferGetDuration(sample), AVB_STREAM_VIDEO_TIMESCALE);
        packet.keyframe = true;
        CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
        if (attachments && CFArrayGetCount(attachments) > 0) {
            auto dict = (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
            CFBooleanRef not_sync = dict
                ? (CFBooleanRef)CFDictionaryGetValue(dict, kCMSampleAttachmentKey_NotSync) : nullptr;
            packet.keyframe = !(not_sync && CFBooleanGetValue(not_sync));
        }
        if (packet.keyframe && m->param_sets.empty()) {
            if (CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample))
                m->param_sets = parameter_sets(format, m->video.params.video.codec, m->nal_length);
        }
        m->queue.push_back(std::move(packet));
    }

    static OSStatus on_audio_input(AudioConverterRef, UInt32 *packets, AudioBufferList *data,
                                   AudioStreamPacketDescription **, void *user) {
        auto *m = static_cast<Impl *>(user);
        const int channels = m->audio.params.audio.channels;
        const UInt32 available = (UInt32)(m->fifo.size() / channels);
        if (available == 0) {
            *packets = 0;
            data->mBuffers[0].mData = nullptr;
            data->mBuffers[0].mDataByteSize = 0;
            return m->final_chunk ? noErr : k_need_more_input;
        }
        const UInt32 n = std::min(*packets, available);
        m->handed.assign(m->fifo.begin(), m->fifo.begin() + (size_t)n * channels);
        m->fifo.erase(m->fifo.begin(), m->fifo.begin() + (size_t)n * channels);
        data->mNumberBuffers = 1;
        data->mBuffers[0].mNumberChannels = (UInt32)channels;
        data->mBuffers[0].mData = m->handed.data();
        data->mBuffers[0].mDataByteSize = (UInt32)(m->handed.size() * sizeof(float));
        *packets = n;
        return noErr;
    }
};

AvbStreamEncoderAVFoundation::AvbStreamEncoderAVFoundation() : m_impl(new Impl()) {}
AvbStreamEncoderAVFoundation::~AvbStreamEncoderAVFoundation() = default;

const char *AvbStreamEncoderAVFoundation::get_last_error() const {
    return m_last_error.empty() ? nullptr : m_last_error.c_str();
}

avb_result AvbStreamEncoderAVFoundation::open_video(const AvbStreamVideoEncodeConfig &config) {
    Impl &m = *m_impl;
    const avb_video_stream_encode_params &p = config.params;
    const avb_video_encode_params &v = p.video;
    m.is_video = true;
    m.video = config;
    if (v.codec != AVB_VIDEO_CODEC_H264 && v.codec != AVB_VIDEO_CODEC_HEVC) {
        m_last_error = "VideoToolbox stream encoding supports H.264 and HEVC.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    const bool external = v.input_memory == AVB_VIDEO_MEMORY_EXTERNAL;
    if (v.input_memory == AVB_VIDEO_MEMORY_BACKEND_NATIVE ||
        (external && v.input_external_type != AVB_VIDEO_EXTERNAL_CVPIXEL_BUFFER)) {
        m_last_error = "VideoToolbox stream encoding takes CPU frames or CVPixelBuffers.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }

    CFScope spec(new_dict());
#if TARGET_OS_OSX
    auto spec_dict = (CFMutableDictionaryRef)spec.get();
    if (v.hardware_policy == AVB_HARDWARE_REQUIRE)
        CFDictionarySetValue(spec_dict, kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder,
                             kCFBooleanTrue);
    else
        CFDictionarySetValue(spec_dict, kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
                             v.hardware_policy == AVB_HARDWARE_DISABLED ? kCFBooleanFalse : kCFBooleanTrue);
#endif
    CFScope source(new_dict());
    if (!external) {
        auto attrs = (CFMutableDictionaryRef)source.get();
        dict_set_int(attrs, kCVPixelBufferPixelFormatTypeKey, (int32_t)cv_pixel_format(v.input_format));
        dict_set_int(attrs, kCVPixelBufferWidthKey, v.width);
        dict_set_int(attrs, kCVPixelBufferHeightKey, v.height);
        CFScope surface(new_dict());
        CFDictionarySetValue(attrs, kCVPixelBufferIOSurfacePropertiesKey, surface.get());
    }
    const CMVideoCodecType codec = v.codec == AVB_VIDEO_CODEC_HEVC ? kCMVideoCodecType_HEVC
                                                                   : kCMVideoCodecType_H264;
    OSStatus status = VTCompressionSessionCreate(
        nullptr, v.width, v.height, codec, (CFDictionaryRef)spec.get(),
        external ? nullptr : (CFDictionaryRef)source.get(), nullptr, &Impl::on_output, &m,
        &m.session);
    if (status != noErr || !m.session) {
        m_last_error = os_message("VTCompressionSessionCreate", status);
        return AVB_ERROR_OPEN_FAILED;
    }
    VTSessionSetProperty(m.session, kVTCompressionPropertyKey_RealTime,
                         p.low_latency ? kCFBooleanTrue : kCFBooleanFalse);
    VTSessionSetProperty(m.session, kVTCompressionPropertyKey_AllowFrameReordering,
                         p.max_b_frames == 0 ? kCFBooleanFalse : kCFBooleanTrue);
    if (p.gop_size > 0) session_set_int(m.session, kVTCompressionPropertyKey_MaxKeyFrameInterval, p.gop_size);
    session_set_int(m.session, kVTCompressionPropertyKey_ExpectedFrameRate, (int32_t)std::lround(v.frame_rate));
    session_set_int(m.session, kVTCompressionPropertyKey_AverageBitRate, v.bitrate);
    const int peak = p.rate_control == AVB_RATE_CONTROL_CBR ? v.bitrate : p.max_bitrate;
    if (peak > 0) {
        // Bytes per one second.
        double bytes = peak / 8.0, seconds = 1.0;
        CFNumberRef n_bytes = CFNumberCreate(nullptr, kCFNumberDoubleType, &bytes);
        CFNumberRef n_seconds = CFNumberCreate(nullptr, kCFNumberDoubleType, &seconds);
        const void *values[] = {n_bytes, n_seconds};
        CFArrayRef limits = CFArrayCreate(nullptr, values, 2, &kCFTypeArrayCallBacks);
        VTSessionSetProperty(m.session, kVTCompressionPropertyKey_DataRateLimits, limits);
        CFRelease(limits);
        CFRelease(n_bytes);
        CFRelease(n_seconds);
    }
    CFStringRef profile = nullptr;
    if (v.codec == AVB_VIDEO_CODEC_HEVC) {
        profile = kVTProfileLevel_HEVC_Main_AutoLevel;
    } else {
        switch (p.profile) {
            case AVB_VIDEO_PROFILE_H264_CONSTRAINED_BASELINE:
                if (@available(macOS 12.0, iOS 15.0, *))
                    profile = kVTProfileLevel_H264_ConstrainedBaseline_AutoLevel;
                else
                    profile = kVTProfileLevel_H264_Baseline_AutoLevel;
                break;
            case AVB_VIDEO_PROFILE_H264_BASELINE: profile = kVTProfileLevel_H264_Baseline_AutoLevel; break;
            case AVB_VIDEO_PROFILE_H264_HIGH: profile = kVTProfileLevel_H264_High_AutoLevel; break;
            default: profile = kVTProfileLevel_H264_Main_AutoLevel; break;
        }
    }
    VTSessionSetProperty(m.session, kVTCompressionPropertyKey_ProfileLevel, profile);
    status = VTCompressionSessionPrepareToEncodeFrames(m.session);
    if (status != noErr) {
        m_last_error = os_message("VTCompressionSessionPrepareToEncodeFrames", status);
        return AVB_ERROR_OPEN_FAILED;
    }
#if TARGET_OS_OSX
    CFBooleanRef hardware = nullptr;
    if (VTSessionCopyProperty(m.session, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,
                              nullptr, &hardware) == noErr && hardware) {
        if (CFBooleanGetValue(hardware)) m_output.hardware_device = AVB_HW_DEVICE_VIDEOTOOLBOX;
        CFRelease(hardware);
    }
#else
    m_output.hardware_device = AVB_HW_DEVICE_VIDEOTOOLBOX;
#endif
    m_output.length_prefixed = true;
    m_output.nal_length_size = 4;
    return AVB_OK;
}

avb_result AvbStreamEncoderAVFoundation::open_audio(const AvbStreamAudioEncodeConfig &config) {
    Impl &m = *m_impl;
    const avb_audio_encode_params &a = config.params.audio;
    m.is_video = false;
    m.audio = config;
    const bool opus = a.codec == AVB_AUDIO_CODEC_OPUS;
    m.frames_per_packet = opus ? (UInt32)(a.sample_rate / 50) : 1024;

    AudioStreamBasicDescription in{};
    in.mSampleRate = a.sample_rate;
    in.mFormatID = kAudioFormatLinearPCM;
    in.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    in.mChannelsPerFrame = (UInt32)a.channels;
    in.mBitsPerChannel = 32;
    in.mFramesPerPacket = 1;
    in.mBytesPerFrame = in.mBytesPerPacket = 4 * (UInt32)a.channels;
    AudioStreamBasicDescription out{};
    out.mSampleRate = a.sample_rate;
    out.mFormatID = opus ? kAudioFormatOpus : kAudioFormatMPEG4AAC;
    out.mFormatFlags = opus ? 0 : kMPEG4Object_AAC_LC;
    out.mChannelsPerFrame = (UInt32)a.channels;
    out.mFramesPerPacket = m.frames_per_packet;
    OSStatus status = AudioConverterNew(&in, &out, &m.converter);
    if (status != noErr) {
        m_last_error = os_message(opus ? "AudioConverterNew (Opus encoder)" : "AudioConverterNew (AAC encoder)",
                                  status);
        return AVB_ERROR_OPEN_FAILED;
    }
    UInt32 bitrate = (UInt32)a.bitrate;
    AudioConverterSetProperty(m.converter, kAudioConverterEncodeBitRate, sizeof(bitrate), &bitrate);
    UInt32 size = sizeof(m.max_packet);
    AudioConverterGetProperty(m.converter, kAudioConverterPropertyMaximumOutputPacketSize, &size,
                              &m.max_packet);
    if (m.max_packet == 0) m.max_packet = 8192;
    AudioConverterPrimeInfo prime{};
    size = sizeof(prime);
    if (AudioConverterGetProperty(m.converter, kAudioConverterPrimeInfo, &size, &prime) == noErr)
        m.leading = (int)prime.leadingFrames;
    m_output.encoder_delay = m.leading;
    if (!opus) {
        UInt32 cookie_size = 0;
        if (AudioConverterGetPropertyInfo(m.converter, kAudioConverterCompressionMagicCookie,
                                          &cookie_size, nullptr) == noErr && cookie_size > 0) {
            std::vector<uint8_t> cookie(cookie_size);
            if (AudioConverterGetProperty(m.converter, kAudioConverterCompressionMagicCookie,
                                          &cookie_size, cookie.data()) == noErr)
                m_output.extradata = avb::detail::aac_asc_from_esds(cookie.data(), cookie_size);
        }
    }
    m.packet_buf.resize(m.max_packet);
    return AVB_OK;
}

avb_result AvbStreamEncoderAVFoundation::send_video(const avb_video_frame &frame, double pts_sec,
                                                    bool force_keyframe) {
    Impl &m = *m_impl;
    if (!m.session) return AVB_ERROR_INVALID_ARGUMENT;
    const avb_video_encode_params &v = m.video.params.video;
    CVPixelBufferRef pb = nullptr;
    if (v.input_memory == AVB_VIDEO_MEMORY_EXTERNAL) {
        if (!frame.native_handle) {
            m_last_error = "CVPixelBuffer input requires a native_handle.";
            return AVB_ERROR_INVALID_ARGUMENT;
        }
        pb = (CVPixelBufferRef)CFRetain(frame.native_handle);
    } else {
        CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(m.session);
        CVReturn cv = pool ? CVPixelBufferPoolCreatePixelBuffer(nullptr, pool, &pb) : kCVReturnError;
        if (cv != kCVReturnSuccess || !pb) {
            m_last_error = "Allocating a VideoToolbox input pixel buffer failed.";
            return AVB_ERROR_ENCODE_FAILED;
        }
        CVPixelBufferLockBaseAddress(pb, 0);
        copy_into_pixel_buffer(frame, pb);
        CVPixelBufferUnlockBaseAddress(pb, 0);
    }
    CFScope props(force_keyframe ? new_dict() : nullptr);
    if (force_keyframe)
        CFDictionarySetValue((CFMutableDictionaryRef)props.get(), kVTEncodeFrameOptionKey_ForceKeyFrame,
                             kCFBooleanTrue);
    const CMTime pts = CMTimeMake(llround(pts_sec * AVB_STREAM_VIDEO_TIMESCALE), AVB_STREAM_VIDEO_TIMESCALE);
    const CMTime duration = CMTimeMake(llround(AVB_STREAM_VIDEO_TIMESCALE / v.frame_rate),
                                       AVB_STREAM_VIDEO_TIMESCALE);
    const OSStatus status = VTCompressionSessionEncodeFrame(
        m.session, pb, pts, duration, (CFDictionaryRef)props.get(), nullptr, nullptr);
    CFRelease(pb);
    if (status != noErr) {
        m_last_error = os_message("VTCompressionSessionEncodeFrame", status);
        return AVB_ERROR_ENCODE_FAILED;
    }
    return AVB_OK;
}

avb_result AvbStreamEncoderAVFoundation::encode_audio(bool final_chunk) {
    Impl &m = *m_impl;
    const int channels = m.audio.params.audio.channels;
    m.final_chunk = final_chunk;
    for (;;) {
        if (!final_chunk && m.fifo.size() < (size_t)m.frames_per_packet * channels) return AVB_OK;
        AudioBufferList list{};
        list.mNumberBuffers = 1;
        list.mBuffers[0].mNumberChannels = (UInt32)channels;
        list.mBuffers[0].mData = m.packet_buf.data();
        list.mBuffers[0].mDataByteSize = (UInt32)m.packet_buf.size();
        UInt32 packets = 1;
        AudioStreamPacketDescription desc{};
        const OSStatus status = AudioConverterFillComplexBuffer(
            m.converter, &Impl::on_audio_input, &m, &packets, &list, &desc);
        if (status != noErr && status != k_need_more_input) {
            m_last_error = os_message("AudioConverterFillComplexBuffer", status);
            return AVB_ERROR_ENCODE_FAILED;
        }
        if (packets == 0) return AVB_OK;
        AvbStreamEncodedPacket packet;
        const uint8_t *data = static_cast<const uint8_t *>(list.mBuffers[0].mData) + desc.mStartOffset;
        const UInt32 bytes = desc.mDataByteSize ? desc.mDataByteSize : list.mBuffers[0].mDataByteSize;
        packet.data.assign(data, data + bytes);
        // Input timeline: the first packets start before 0 by the priming.
        packet.pts = m.packets_out * m.frames_per_packet - m.leading;
        packet.duration = m.frames_per_packet;
        packet.keyframe = true;
        ++m.packets_out;
        {
            std::lock_guard<std::mutex> lock(m.mutex);
            m.queue.push_back(std::move(packet));
        }
        if (status == k_need_more_input) return AVB_OK;
    }
}

avb_result AvbStreamEncoderAVFoundation::send_audio(const float *interleaved, int frames) {
    Impl &m = *m_impl;
    if (!m.converter) return AVB_ERROR_INVALID_ARGUMENT;
    m.fifo.insert(m.fifo.end(), interleaved,
                  interleaved + (size_t)frames * m.audio.params.audio.channels);
    return encode_audio(false);
}

avb_result AvbStreamEncoderAVFoundation::receive(AvbStreamEncodedPacket &out) {
    Impl &m = *m_impl;
    std::lock_guard<std::mutex> lock(m.mutex);
    if (m.callback_status != noErr) {
        m_last_error = os_message("VideoToolbox encoding", m.callback_status);
        return AVB_ERROR_ENCODE_FAILED;
    }
    if (m.is_video && m_output.extradata.empty() && !m.param_sets.empty()) {
        m_output.extradata = m.param_sets;
        m_output.nal_length_size = m.nal_length;
    }
    if (m.queue.empty()) return m.flushed ? AVB_ERROR_EOF : AVB_ERROR_AGAIN;
    out = std::move(m.queue.front());
    m.queue.pop_front();
    return AVB_OK;
}

avb_result AvbStreamEncoderAVFoundation::set_bitrate(int bitrate) {
    Impl &m = *m_impl;
    if (m.session) {
        const OSStatus status = session_set_int(m.session, kVTCompressionPropertyKey_AverageBitRate, bitrate);
        if (status != noErr) {
            m_last_error = os_message("Setting kVTCompressionPropertyKey_AverageBitRate", status);
            return AVB_ERROR_ENCODE_FAILED;
        }
        return AVB_OK;
    }
    if (m.converter) {
        UInt32 value = (UInt32)bitrate;
        const OSStatus status =
            AudioConverterSetProperty(m.converter, kAudioConverterEncodeBitRate, sizeof(value), &value);
        if (status != noErr) {
            m_last_error = os_message("Setting kAudioConverterEncodeBitRate", status);
            return AVB_ERROR_ENCODE_FAILED;
        }
        return AVB_OK;
    }
    return AVB_ERROR_INVALID_ARGUMENT;
}

avb_result AvbStreamEncoderAVFoundation::flush() {
    Impl &m = *m_impl;
    if (m.session) {
        const OSStatus status = VTCompressionSessionCompleteFrames(m.session, kCMTimeInvalid);
        if (status != noErr) {
            m_last_error = os_message("VTCompressionSessionCompleteFrames", status);
            return AVB_ERROR_ENCODE_FAILED;
        }
    } else if (m.converter) {
        const avb_result res = encode_audio(true);
        if (res != AVB_OK) return res;
    } else {
        return AVB_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(m.mutex);
    m.flushed = true;
    return AVB_OK;
}

/* ================================================================ decoder */

struct AvbStreamDecoderAVFoundation::Impl {
    AvbStreamDecodeConfig config;
    bool is_video = false;
    VTDecompressionSessionRef session = nullptr;
    CMFormatDescriptionRef format = nullptr;
    std::mutex mutex;
    struct Decoded {
        CVImageBufferRef image;
        double pts;
    };
    std::deque<Decoded> frames;
    OSStatus callback_status = noErr;
    bool draining = false;
    bool hardware = false;
    std::vector<uint8_t> frame_buf;

    AudioConverterRef converter = nullptr;
    const uint8_t *pending = nullptr;
    UInt32 pending_size = 0;
    AudioStreamPacketDescription pending_desc{};
    int sample_rate = 0, channels = 0;
    UInt32 max_frames = 8192;
    struct Chunk {
        std::vector<float> samples;
        int frames;
        double pts;
    };
    std::deque<Chunk> chunks;

    ~Impl() {
        close_session();
        if (format) CFRelease(format);
        if (converter) AudioConverterDispose(converter);
    }

    void clear_frames() {
        std::lock_guard<std::mutex> lock(mutex);
        for (Decoded &d : frames) CVBufferRelease(d.image);
        frames.clear();
    }

    void close_session() {
        if (session) {
            VTDecompressionSessionWaitForAsynchronousFrames(session);
            VTDecompressionSessionInvalidate(session);
            CFRelease(session);
            session = nullptr;
        }
        clear_frames();
    }

    static void on_output(void *refcon, void *, OSStatus status, VTDecodeInfoFlags,
                          CVImageBufferRef image, CMTime pts, CMTime) {
        auto *m = static_cast<Impl *>(refcon);
        std::lock_guard<std::mutex> lock(m->mutex);
        if (status != noErr) {
            m->callback_status = status;
            return;
        }
        if (!image) return;
        m->frames.push_back({CVBufferRetain(image), CMTIME_IS_VALID(pts) ? CMTimeGetSeconds(pts) : -1.0});
    }

    static OSStatus on_audio_input(AudioConverterRef, UInt32 *packets, AudioBufferList *data,
                                   AudioStreamPacketDescription **desc, void *user) {
        auto *m = static_cast<Impl *>(user);
        if (!m->pending) {
            *packets = 0;
            return k_need_more_input;
        }
        data->mNumberBuffers = 1;
        data->mBuffers[0].mNumberChannels = (UInt32)m->channels;
        data->mBuffers[0].mData = const_cast<uint8_t *>(m->pending);
        data->mBuffers[0].mDataByteSize = m->pending_size;
        m->pending_desc.mStartOffset = 0;
        m->pending_desc.mVariableFramesInPacket = 0;
        m->pending_desc.mDataByteSize = m->pending_size;
        if (desc) *desc = &m->pending_desc;
        *packets = 1;
        m->pending = nullptr;
        return noErr;
    }
};

AvbStreamDecoderAVFoundation::AvbStreamDecoderAVFoundation() : m_impl(new Impl()) {}
AvbStreamDecoderAVFoundation::~AvbStreamDecoderAVFoundation() = default;

const char *AvbStreamDecoderAVFoundation::get_last_error() const {
    return m_last_error.empty() ? nullptr : m_last_error.c_str();
}

void AvbStreamDecoderAVFoundation::audio_format(int &sample_rate, int &channels) const {
    sample_rate = m_impl->sample_rate;
    channels = m_impl->channels;
}

avb_hardware_device AvbStreamDecoderAVFoundation::hardware_device() const {
    return m_impl->hardware ? AVB_HW_DEVICE_VIDEOTOOLBOX : AVB_HW_DEVICE_AUTO;
}

avb_result AvbStreamDecoderAVFoundation::create_session(const std::vector<uint8_t> &config) {
    Impl &m = *m_impl;
    const avb_stream_decode_params &p = m.config.params;
    avb::detail::ParamSets ps(p.video_codec);
    ps.load_extradata(config.data(), config.size());
    if (!ps.complete()) {
        m_last_error = "VideoToolbox needs the stream's parameter sets.";
        return AVB_ERROR_DECODE_FAILED;
    }
    std::vector<const uint8_t *> pointers;
    std::vector<size_t> sizes;
    for (const auto *nal : ps.all()) {
        pointers.push_back(nal->data());
        sizes.push_back(nal->size());
    }
    CMFormatDescriptionRef format = nullptr;
    OSStatus status;
    if (p.video_codec == AVB_VIDEO_CODEC_HEVC) {
        if (@available(macOS 10.13, iOS 11.0, *))
            status = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                nullptr, pointers.size(), pointers.data(), sizes.data(), 4, nullptr, &format);
        else
            status = kVTCouldNotFindVideoDecoderErr;
    } else {
        status = CMVideoFormatDescriptionCreateFromH264ParameterSets(
            nullptr, pointers.size(), pointers.data(), sizes.data(), 4, &format);
    }
    if (status != noErr || !format) {
        m_last_error = os_message("Creating the video format description", status);
        return AVB_ERROR_DECODE_FAILED;
    }
    // A new SPS with the same layout keeps the session.
    if (m.session && VTDecompressionSessionCanAcceptFormatDescription(m.session, format)) {
        if (m.format) CFRelease(m.format);
        m.format = format;
        return AVB_OK;
    }
    m.close_session();
    if (m.format) CFRelease(m.format);
    m.format = format;

    CFScope spec(new_dict());
#if TARGET_OS_OSX
    if (p.hardware_policy == AVB_HARDWARE_REQUIRE)
        CFDictionarySetValue((CFMutableDictionaryRef)spec.get(),
                             kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder,
                             kCFBooleanTrue);
    else if (p.hardware_policy == AVB_HARDWARE_PREFER)
        CFDictionarySetValue((CFMutableDictionaryRef)spec.get(),
                             kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder,
                             kCFBooleanTrue);
#endif
    CFScope attrs(new_dict());
    const bool native = p.video_memory == AVB_VIDEO_MEMORY_EXTERNAL;
    dict_set_int((CFMutableDictionaryRef)attrs.get(), kCVPixelBufferPixelFormatTypeKey,
                 (int32_t)cv_pixel_format(native ? AVB_PIXEL_FORMAT_NV12 : p.video_format));
    CFScope surface(new_dict());
    CFDictionarySetValue((CFMutableDictionaryRef)attrs.get(), kCVPixelBufferIOSurfacePropertiesKey,
                         surface.get());
    VTDecompressionOutputCallbackRecord callback{&Impl::on_output, &m};
    status = VTDecompressionSessionCreate(nullptr, format, (CFDictionaryRef)spec.get(),
                                          (CFDictionaryRef)attrs.get(), &callback, &m.session);
    if (status != noErr || !m.session) {
        m_last_error = os_message("VTDecompressionSessionCreate", status);
        return AVB_ERROR_OPEN_FAILED;
    }
    if (p.low_latency) VTSessionSetProperty(m.session, kVTDecompressionPropertyKey_RealTime, kCFBooleanTrue);
    m.hardware = false;
#if TARGET_OS_OSX
    CFBooleanRef hardware = nullptr;
    if (VTSessionCopyProperty(m.session, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,
                              nullptr, &hardware) == noErr && hardware) {
        m.hardware = CFBooleanGetValue(hardware);
        CFRelease(hardware);
    }
#else
    m.hardware = true;
#endif
    return AVB_OK;
}

avb_result AvbStreamDecoderAVFoundation::open(const AvbStreamDecodeConfig &config) {
    Impl &m = *m_impl;
    m.config = config;
    const avb_stream_decode_params &p = config.params;
    m.is_video = p.is_video != 0;
    if (m.is_video) {
        if (p.video_codec != AVB_VIDEO_CODEC_H264 && p.video_codec != AVB_VIDEO_CODEC_HEVC) {
            m_last_error = "VideoToolbox stream decoding supports H.264 and HEVC.";
            return AVB_ERROR_INVALID_ARGUMENT;
        }
        if (p.video_memory == AVB_VIDEO_MEMORY_BACKEND_NATIVE ||
            (p.video_memory == AVB_VIDEO_MEMORY_EXTERNAL &&
             p.video_external_type != AVB_VIDEO_EXTERNAL_CVPIXEL_BUFFER)) {
            m_last_error = "VideoToolbox stream decoding outputs CPU frames or CVPixelBuffers.";
            return AVB_ERROR_INVALID_ARGUMENT;
        }
        return config.extradata.empty() ? AVB_OK : create_session(config.extradata);
    }

    const bool opus = p.audio_codec == AVB_AUDIO_CODEC_OPUS;
    m.sample_rate = p.sample_rate;
    m.channels = p.channels;
    AudioStreamBasicDescription in{};
    in.mSampleRate = opus ? 48000 : p.sample_rate;
    in.mFormatID = opus ? kAudioFormatOpus : kAudioFormatMPEG4AAC;
    in.mChannelsPerFrame = (UInt32)p.channels;
    in.mFramesPerPacket = opus ? 960 : 1024;
    AudioStreamBasicDescription out{};
    out.mSampleRate = in.mSampleRate;
    out.mFormatID = kAudioFormatLinearPCM;
    out.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    out.mChannelsPerFrame = (UInt32)p.channels;
    out.mBitsPerChannel = 32;
    out.mFramesPerPacket = 1;
    out.mBytesPerFrame = out.mBytesPerPacket = 4 * (UInt32)p.channels;
    m.sample_rate = (int)in.mSampleRate;
    OSStatus status = AudioConverterNew(&in, &out, &m.converter);
    if (status != noErr) {
        m_last_error = os_message(opus ? "AudioConverterNew (Opus decoder)" : "AudioConverterNew (AAC decoder)",
                                  status);
        return AVB_ERROR_OPEN_FAILED;
    }
    if (!opus && !config.extradata.empty()) {
        const std::vector<uint8_t> esds =
            avb::detail::aac_esds_from_asc(config.extradata.data(), config.extradata.size());
        AudioConverterSetProperty(m.converter, kAudioConverterDecompressionMagicCookie,
                                  (UInt32)esds.size(), esds.data());
    }
    return AVB_OK;
}

avb_result AvbStreamDecoderAVFoundation::send(const AvbStreamDecodeInput &input) {
    Impl &m = *m_impl;
    if (!input.data) {
        m.draining = true;
        if (m.session) {
            VTDecompressionSessionFinishDelayedFrames(m.session);
            VTDecompressionSessionWaitForAsynchronousFrames(m.session);
        }
        return AVB_OK;
    }
    if (!m.is_video) {
        m.pending = input.data;
        m.pending_size = (UInt32)input.size;
        std::vector<float> samples((size_t)m.max_frames * std::max(1, m.channels));
        AudioBufferList list{};
        list.mNumberBuffers = 1;
        list.mBuffers[0].mNumberChannels = (UInt32)m.channels;
        list.mBuffers[0].mData = samples.data();
        list.mBuffers[0].mDataByteSize = (UInt32)(samples.size() * sizeof(float));
        UInt32 frames = m.max_frames;
        const OSStatus status =
            AudioConverterFillComplexBuffer(m.converter, &Impl::on_audio_input, &m, &frames, &list, nullptr);
        m.pending = nullptr;
        if (status != noErr && status != k_need_more_input) {
            m_last_error = os_message("AudioConverterFillComplexBuffer (decode)", status);
            return AVB_ERROR_DECODE_FAILED;
        }
        if (frames > 0) {
            samples.resize((size_t)frames * m.channels);
            m.chunks.push_back({std::move(samples), (int)frames, input.pts_sec});
        }
        return AVB_OK;
    }

    if (input.config_changed || !m.session) {
        if (!input.config || input.config->empty()) {
            m_last_error = "VideoToolbox needs the stream's parameter sets.";
            return AVB_ERROR_DECODE_FAILED;
        }
        const avb_result res = create_session(*input.config);
        if (res != AVB_OK) return res;
    }
    CMBlockBufferRef block = nullptr;
    OSStatus status = CMBlockBufferCreateWithMemoryBlock(nullptr, nullptr, input.size, kCFAllocatorDefault,
                                                         nullptr, 0, input.size,
                                                         kCMBlockBufferAssureMemoryNowFlag, &block);
    if (status == noErr) status = CMBlockBufferReplaceDataBytes(input.data, block, 0, input.size);
    CMSampleBufferRef sample = nullptr;
    if (status == noErr) {
        CMSampleTimingInfo timing{kCMTimeInvalid, kCMTimeInvalid, kCMTimeInvalid};
        if (input.pts_sec >= 0.0)
            timing.presentationTimeStamp =
                CMTimeMake(llround(input.pts_sec * AVB_STREAM_VIDEO_TIMESCALE), AVB_STREAM_VIDEO_TIMESCALE);
        const size_t size = input.size;
        status = CMSampleBufferCreateReady(nullptr, block, m.format, 1, 1, &timing, 1, &size, &sample);
    }
    if (block) CFRelease(block);
    if (status != noErr || !sample) {
        m_last_error = os_message("Creating the decoder sample buffer", status);
        return AVB_ERROR_DECODE_FAILED;
    }
    // Temporal processing emits frames in presentation order; low latency
    // decodes synchronously, frame by frame.
    const VTDecodeFrameFlags flags = m.config.params.low_latency
        ? 0 : (kVTDecodeFrame_EnableAsynchronousDecompression | kVTDecodeFrame_EnableTemporalProcessing);
    VTDecodeInfoFlags info = 0;
    status = VTDecompressionSessionDecodeFrame(m.session, sample, flags, nullptr, &info);
    CFRelease(sample);
    if (status != noErr) {
        m_last_error = os_message("VTDecompressionSessionDecodeFrame", status);
        return AVB_ERROR_DECODE_FAILED;
    }
    return AVB_OK;
}

avb_result AvbStreamDecoderAVFoundation::receive_video(avb_video_frame &out) {
    Impl &m = *m_impl;
    if (!m.is_video) return AVB_ERROR_INVALID_ARGUMENT;
    Impl::Decoded decoded{};
    {
        std::lock_guard<std::mutex> lock(m.mutex);
        if (m.callback_status != noErr) {
            const OSStatus status = m.callback_status;
            m.callback_status = noErr;
            m_last_error = os_message("VideoToolbox decoding", status);
            return AVB_ERROR_DECODE_FAILED;
        }
        if (m.frames.empty()) return m.draining ? AVB_ERROR_EOF : AVB_ERROR_AGAIN;
        decoded = m.frames.front();
        m.frames.pop_front();
    }
    const avb_stream_decode_params &p = m.config.params;
    CVPixelBufferRef pb = decoded.image;
    const int w = (int)CVPixelBufferGetWidth(pb);
    const int h = (int)CVPixelBufferGetHeight(pb);
    CFTypeRef matrix = CVBufferGetAttachment(pb, kCVImageBufferYCbCrMatrixKey, nullptr);
    const bool bt709 = matrix ? CFEqual(matrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2)
                              : avb::detail::use_bt709(w, h);
    const OSType type = CVPixelBufferGetPixelFormatType(pb);
    const bool full = type == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ||
                      type == kCVPixelFormatType_420YpCbCr8PlanarFullRange;

    if (p.video_memory == AVB_VIDEO_MEMORY_EXTERNAL) {
        avb::detail::init_cpu_frame(out, w, h, AVB_PIXEL_FORMAT_NV12, decoded.pts);
        out.memory_type = AVB_VIDEO_MEMORY_EXTERNAL;
        out.external_type = AVB_VIDEO_EXTERNAL_CVPIXEL_BUFFER;
        out.hardware_device = hardware_device();
        out.native_handle = (void *)pb;
        out.native_owner = (void *)pb; // released by release_video
        out.color_range = full ? AVB_COLOR_RANGE_FULL : AVB_COLOR_RANGE_LIMITED;
        out.color_matrix = bt709 ? AVB_COLOR_MATRIX_BT709 : AVB_COLOR_MATRIX_BT601;
        return AVB_OK;
    }

    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    if (CVPixelBufferIsPlanar(pb)) {
        avb::detail::Yuv420Planes planes;
        planes.y = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pb, 0);
        planes.y_stride = (int)CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
        planes.u = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pb, 1);
        planes.u_stride = (int)CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
        planes.nv12 = CVPixelBufferGetPlaneCount(pb) == 2;
        if (!planes.nv12) {
            planes.v = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pb, 2);
            planes.v_stride = (int)CVPixelBufferGetBytesPerRowOfPlane(pb, 2);
        }
        avb::detail::yuv420_to_frame(planes, w, h, p.video_format, bt709, full, decoded.pts,
                                     m.frame_buf, out);
    } else {
        avb::detail::bgra_to_frame((const uint8_t *)CVPixelBufferGetBaseAddress(pb),
                                   (int)CVPixelBufferGetBytesPerRow(pb), w, h, p.video_format,
                                   decoded.pts, m.frame_buf, out);
    }
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    CVBufferRelease(pb);
    out.hardware_device = hardware_device();
    return AVB_OK;
}

void AvbStreamDecoderAVFoundation::release_video(avb_video_frame &frame) {
    if (frame.memory_type == AVB_VIDEO_MEMORY_EXTERNAL && frame.native_owner) {
        CVBufferRelease((CVBufferRef)frame.native_owner);
        frame.native_owner = nullptr;
    }
}

avb_result AvbStreamDecoderAVFoundation::receive_audio(std::vector<float> &out, int &frames,
                                                       double &pts_sec) {
    Impl &m = *m_impl;
    frames = 0;
    pts_sec = -1.0;
    if (m.is_video) return AVB_ERROR_INVALID_ARGUMENT;
    if (m.chunks.empty()) return m.draining ? AVB_ERROR_EOF : AVB_ERROR_AGAIN;
    Impl::Chunk chunk = std::move(m.chunks.front());
    m.chunks.pop_front();
    out = std::move(chunk.samples);
    frames = chunk.frames;
    pts_sec = chunk.pts;
    return AVB_OK;
}

avb_result AvbStreamDecoderAVFoundation::reset() {
    Impl &m = *m_impl;
    m.draining = false;
    if (m.session) VTDecompressionSessionWaitForAsynchronousFrames(m.session);
    m.clear_frames();
    m.chunks.clear();
    if (m.converter) AudioConverterReset(m.converter);
    return AVB_OK;
}
