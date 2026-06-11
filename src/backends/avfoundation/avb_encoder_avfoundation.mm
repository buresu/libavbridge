#include "avb_encoder_avfoundation.hh"
#include "../../avb_video_codec_registry.hpp"

#ifdef __APPLE__

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <unistd.h>
#include <algorithm>
#include <cstring>
#include <deque>
#include <utility>

struct AvbEncoderAVFoundation::Impl {
    AVAssetWriter *writer = nil;
    AVAssetWriterInput *video_input = nil;
    AVAssetWriterInputPixelBufferAdaptor *video_adaptor = nil;
    AVAssetWriterInput *audio_input = nil;

    CMAudioFormatDescriptionRef audio_fmt = nullptr;
    CMVideoFormatDescriptionRef custom_video_fmt = nullptr;
    const avb_video_encoder_plugin *custom_video_encoder = nullptr;
    void *custom_video_ctx = nullptr;
    avb_encoded_video_stream custom_video_stream{};

    bool   has_video = false;
    bool   has_audio = false;
    bool   custom_video = false;
    int    width = 0, height = 0;
    double frame_rate = 30.0;
    OSType cv_pixel_format = kCVPixelFormatType_32BGRA;
    avb_pixel_format input_format = AVB_PIXEL_FORMAT_BGRA8;

    int    sample_rate = 0;
    int    channels = 0;

    long   video_frame_index = 0; // for auto PTS
    long   audio_sample_count = 0; // running total for audio PTS

    bool   finished = false;

    // Pending samples not yet accepted by the writer. The two inputs are drained
    // independently so that one input being temporarily not-ready never blocks
    // the other (which would deadlock: the writer wants the other track fed).
    std::deque<std::pair<CVPixelBufferRef, CMTime>> video_q;
    std::deque<CMSampleBufferRef>                   custom_video_q;
    std::deque<CMSampleBufferRef>                   audio_q;

    ~Impl() {
        for (auto &v : video_q) CVPixelBufferRelease(v.first);
        for (auto sb : custom_video_q) CFRelease(sb);
        for (auto sb : audio_q) CFRelease(sb);
        if (audio_fmt) CFRelease(audio_fmt);
        if (custom_video_fmt) CFRelease(custom_video_fmt);
        if (custom_video_encoder) {
            if (custom_video_encoder->close && custom_video_ctx)
                custom_video_encoder->close(custom_video_ctx);
            custom_video_encoder = nullptr;
            custom_video_ctx = nullptr;
        }
    }
};

AvbEncoderAVFoundation::AvbEncoderAVFoundation() { m_impl = new Impl(); }

AvbEncoderAVFoundation::~AvbEncoderAVFoundation() {
    if (m_impl) {
        // Cancel any unfinished writing session to avoid leaving a half-written
        // file locked by the writer.
        if (m_impl->writer && !m_impl->finished &&
            m_impl->writer.status == AVAssetWriterStatusWriting) {
            [m_impl->writer cancelWriting];
        }
        delete m_impl;
    }
}

const char *AvbEncoderAVFoundation::get_last_error() const {
    return m_last_error.empty() ? nullptr : m_last_error.c_str();
}

static OSType avb_enc_cvpixfmt(avb_pixel_format fmt, bool *ok) {
    *ok = true;
    switch (fmt) {
        case AVB_PIXEL_FORMAT_NV12:
            return kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        case AVB_PIXEL_FORMAT_I420:
            return kCVPixelFormatType_420YpCbCr8Planar;
        case AVB_PIXEL_FORMAT_BGRA8:
        case AVB_PIXEL_FORMAT_UNKNOWN:
            return kCVPixelFormatType_32BGRA;
        default:
            *ok = false; // RGBA8 etc. not supported as encoder input
            return kCVPixelFormatType_32BGRA;
    }
}

static AVFileType avb_file_type_for_path(const char *path) {
    NSString *p = [NSString stringWithUTF8String:path];
    NSString *ext = p.pathExtension.lowercaseString;
    if ([ext isEqualToString:@"mov"]) return AVFileTypeQuickTimeMovie;
    if ([ext isEqualToString:@"m4a"]) return AVFileTypeAppleM4A;
    return AVFileTypeMPEG4; // .mp4 and default
}

static bool avb_is_compressed_format(avb_pixel_format fmt) {
    return fmt == AVB_PIXEL_FORMAT_BC1_RGBA ||
           fmt == AVB_PIXEL_FORMAT_BC3_RGBA ||
           fmt == AVB_PIXEL_FORMAT_BC4_R ||
           fmt == AVB_PIXEL_FORMAT_BC5_RG ||
           fmt == AVB_PIXEL_FORMAT_BC7_RGBA;
}

static FourCharCode avb_video_codec_fourcc(avb_video_codec codec) {
    switch (codec) {
        case AVB_VIDEO_CODEC_H264: return kCMVideoCodecType_H264;
        case AVB_VIDEO_CODEC_HEVC: return kCMVideoCodecType_HEVC;
        case AVB_VIDEO_CODEC_VP8:  return 'vp08';
        case AVB_VIDEO_CODEC_VP9:  return 'vp09';
        case AVB_VIDEO_CODEC_AV1:  return 'av01';
        case AVB_VIDEO_CODEC_HAP:  return 'Hap1';
        default:             return 0;
    }
}

static FourCharCode avb_normalize_fourcc(uint32_t tag) {
    if (tag == 0) return 0;
    char be[4] = {
        (char)((tag >> 24) & 0xff),
        (char)((tag >> 16) & 0xff),
        (char)((tag >> 8) & 0xff),
        (char)(tag & 0xff),
    };
    bool be_printable = true;
    for (char c : be) {
        if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e) be_printable = false;
    }
    if (be_printable) return (FourCharCode)tag;
    return ((tag & 0x000000ffu) << 24) |
           ((tag & 0x0000ff00u) << 8)  |
           ((tag & 0x00ff0000u) >> 8)  |
           ((tag & 0xff000000u) >> 24);
}

avb_result AvbEncoderAVFoundation::open(const char *path, const avb_encode_options &options) {
    @autoreleasepool {
        if (options.video.enable &&
            (options.video.input_memory != AVB_VIDEO_MEMORY_CPU ||
             options.video.hardware_policy == AVB_HARDWARE_REQUIRE)) {
            m_last_error = "AVFoundation native hardware video input is not implemented yet.";
            return AVB_ERROR_OPEN_FAILED;
        }
        NSString *ns_path = [NSString stringWithUTF8String:path];
        NSURL *url = [NSURL fileURLWithPath:ns_path];

        // AVAssetWriter refuses to overwrite; remove any existing file first.
        [[NSFileManager defaultManager] removeItemAtURL:url error:nil];

        NSError *err = nil;
        m_impl->writer = [AVAssetWriter assetWriterWithURL:url
                                                  fileType:avb_file_type_for_path(path)
                                                     error:&err];
        if (!m_impl->writer || err) {
            m_last_error = "AVAssetWriter creation failed.";
            return AVB_ERROR_OPEN_FAILED;
        }

        // --- Video input ---
        if (options.video.enable) {
            if (options.video.width <= 0 || options.video.height <= 0) {
                m_last_error = "Video width/height must be positive.";
                return AVB_ERROR_INVALID_ARGUMENT;
            }
            m_impl->input_format = options.video.input_format == AVB_PIXEL_FORMAT_UNKNOWN
                ? AVB_PIXEL_FORMAT_BGRA8 : options.video.input_format;
            m_impl->width      = options.video.width;
            m_impl->height     = options.video.height;
            m_impl->frame_rate = options.video.frame_rate > 0 ? options.video.frame_rate : 30.0;

            avb_video_encode_info custom_info{};
            custom_info.width = m_impl->width;
            custom_info.height = m_impl->height;
            custom_info.frame_rate = m_impl->frame_rate;
            custom_info.input_format = m_impl->input_format;
            custom_info.input_memory = options.video.input_memory;
            custom_info.codec = options.video.codec;
            custom_info.bitrate = options.video.bitrate;
            const avb_video_encoder_plugin *plugin =
                avb_find_video_encoder_plugin(custom_info);

            if (plugin) {
                void *ctx = nullptr;
                avb_encoded_video_stream stream{};
                avb_result cres = plugin->open(&ctx, &custom_info, &stream);
                if (cres != AVB_OK) {
                    m_last_error = "Custom video encoder failed to open.";
                    return cres;
                }

                FourCharCode codec_type = avb_normalize_fourcc(stream.codec_tag);
                if (codec_type == 0) codec_type = avb_video_codec_fourcc(stream.codec);
                if (codec_type == 0) codec_type = avb_video_codec_fourcc(options.video.codec);
                if (codec_type == 0) {
                    m_last_error = "Custom AVFoundation video encoder requires codec_tag.";
                    if (plugin->close) plugin->close(ctx);
                    return AVB_ERROR_INVALID_ARGUMENT;
                }

                OSStatus st = CMVideoFormatDescriptionCreate(
                    kCFAllocatorDefault,
                    codec_type,
                    m_impl->width,
                    m_impl->height,
                    nullptr,
                    &m_impl->custom_video_fmt);
                if (st != noErr || !m_impl->custom_video_fmt) {
                    m_last_error = "CMVideoFormatDescriptionCreate failed.";
                    if (plugin->close) plugin->close(ctx);
                    return AVB_ERROR_OPEN_FAILED;
                }

                m_impl->video_input = [AVAssetWriterInput
                    assetWriterInputWithMediaType:AVMediaTypeVideo
                                   outputSettings:nil
                                 sourceFormatHint:m_impl->custom_video_fmt];
                m_impl->video_input.expectsMediaDataInRealTime = NO;
                m_impl->custom_video_encoder = plugin;
                m_impl->custom_video_ctx = ctx;
                m_impl->custom_video_stream = stream;
                m_impl->custom_video = true;
            } else {
                bool fmt_ok = true;
                m_impl->cv_pixel_format = avb_enc_cvpixfmt(m_impl->input_format, &fmt_ok);
                if (!fmt_ok) {
                    m_last_error = "Unsupported video input_format (use BGRA8, NV12 or I420).";
                    return AVB_ERROR_INVALID_ARGUMENT;
                }
                if (options.video.codec == AVB_VIDEO_CODEC_HAP) {
                    m_last_error = "HAP encoding requires a registered custom video encoder.";
                    return AVB_ERROR_INVALID_ARGUMENT;
                }
                if (avb_is_compressed_format(m_impl->input_format)) {
                    m_last_error = "Compressed video input requires a registered custom video encoder.";
                    return AVB_ERROR_INVALID_ARGUMENT;
                }

                // AVAssetWriter encodes H.264 and HEVC; it cannot produce
                // VP8/VP9/AV1 through the built-in writer settings.
                AVVideoCodecType vcodec;
                switch (options.video.codec) {
                    case AVB_VIDEO_CODEC_AUTO:
                    case AVB_VIDEO_CODEC_H264: vcodec = AVVideoCodecTypeH264; break;
                    case AVB_VIDEO_CODEC_HEVC: vcodec = AVVideoCodecTypeHEVC; break;
                    case AVB_VIDEO_CODEC_VP8:
                        m_last_error = "AVFoundation cannot encode VP8 (use H264, HEVC, FFmpeg, or GStreamer).";
                        return AVB_ERROR_INVALID_ARGUMENT;
                    case AVB_VIDEO_CODEC_VP9:
                        m_last_error = "AVFoundation cannot encode VP9 (use H264, HEVC, FFmpeg, or GStreamer).";
                        return AVB_ERROR_INVALID_ARGUMENT;
                    case AVB_VIDEO_CODEC_AV1:
                        m_last_error = "AVFoundation cannot encode AV1 (use H264, HEVC, FFmpeg, or GStreamer).";
                        return AVB_ERROR_INVALID_ARGUMENT;
                    default:
                        m_last_error = "Invalid video codec (use AUTO/H264/HEVC/HAP).";
                        return AVB_ERROR_INVALID_ARGUMENT;
                }

                NSMutableDictionary *vsettings = [@{
                    AVVideoCodecKey:  vcodec,
                    AVVideoWidthKey:  @(m_impl->width),
                    AVVideoHeightKey: @(m_impl->height),
                } mutableCopy];
                if (options.video.bitrate > 0) {
                    vsettings[AVVideoCompressionPropertiesKey] =
                        @{ AVVideoAverageBitRateKey: @(options.video.bitrate) };
                }

                m_impl->video_input = [AVAssetWriterInput
                    assetWriterInputWithMediaType:AVMediaTypeVideo
                                   outputSettings:vsettings];
                m_impl->video_input.expectsMediaDataInRealTime = NO;

                NSDictionary *src_attrs = @{
                    (NSString *)kCVPixelBufferPixelFormatTypeKey: @(m_impl->cv_pixel_format),
                    (NSString *)kCVPixelBufferWidthKey:           @(m_impl->width),
                    (NSString *)kCVPixelBufferHeightKey:          @(m_impl->height),
                };
                m_impl->video_adaptor = [AVAssetWriterInputPixelBufferAdaptor
                    assetWriterInputPixelBufferAdaptorWithAssetWriterInput:m_impl->video_input
                                               sourcePixelBufferAttributes:src_attrs];
            }

            if (![m_impl->writer canAddInput:m_impl->video_input]) {
                m_last_error = "AVAssetWriter cannot add video input.";
                return AVB_ERROR_OPEN_FAILED;
            }
            [m_impl->writer addInput:m_impl->video_input];
            m_impl->has_video = true;
        }

        // --- Audio input ---
        if (options.audio.enable) {
            if (options.audio.sample_rate <= 0 || options.audio.channels <= 0) {
                m_last_error = "Audio sample_rate/channels must be positive.";
                return AVB_ERROR_INVALID_ARGUMENT;
            }
            m_impl->sample_rate = options.audio.sample_rate;
            m_impl->channels    = options.audio.channels;

            // AVAssetWriter into .mp4/.mov/.m4a produces AAC; the expanded
            // audio codec set is provided by FFmpeg/GStreamer for now.
            switch (options.audio.codec) {
                case AVB_AUDIO_CODEC_AUTO:
                case AVB_AUDIO_CODEC_AAC: break;
                case AVB_AUDIO_CODEC_OPUS:
                case AVB_AUDIO_CODEC_MP3:
                case AVB_AUDIO_CODEC_FLAC:
                case AVB_AUDIO_CODEC_VORBIS:
                case AVB_AUDIO_CODEC_PCM_S16:
                case AVB_AUDIO_CODEC_PCM_F32:
                    m_last_error = "Requested audio codec is not supported by AVFoundation yet (use AAC, FFmpeg, or GStreamer).";
                    return AVB_ERROR_INVALID_ARGUMENT;
                default:
                    m_last_error = "Invalid audio codec.";
                    return AVB_ERROR_INVALID_ARGUMENT;
            }

            NSDictionary *asettings = @{
                AVFormatIDKey:         @(kAudioFormatMPEG4AAC),
                AVSampleRateKey:       @(m_impl->sample_rate),
                AVNumberOfChannelsKey: @(m_impl->channels),
                AVEncoderBitRateKey:   @(options.audio.bitrate > 0 ? options.audio.bitrate : 128000),
            };
            m_impl->audio_input = [AVAssetWriterInput
                assetWriterInputWithMediaType:AVMediaTypeAudio
                               outputSettings:asettings];
            m_impl->audio_input.expectsMediaDataInRealTime = NO;

            // Format description for the interleaved float32 PCM we will feed in.
            AudioStreamBasicDescription asbd = {0};
            asbd.mSampleRate       = m_impl->sample_rate;
            asbd.mFormatID         = kAudioFormatLinearPCM;
            asbd.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
            asbd.mChannelsPerFrame = m_impl->channels;
            asbd.mBitsPerChannel   = 32;
            asbd.mBytesPerFrame    = m_impl->channels * sizeof(float);
            asbd.mFramesPerPacket  = 1;
            asbd.mBytesPerPacket   = asbd.mBytesPerFrame;
            OSStatus st = CMAudioFormatDescriptionCreate(kCFAllocatorDefault, &asbd,
                                                         0, NULL, 0, NULL, NULL,
                                                         &m_impl->audio_fmt);
            if (st != noErr) {
                m_last_error = "CMAudioFormatDescriptionCreate failed.";
                return AVB_ERROR_OPEN_FAILED;
            }

            if (![m_impl->writer canAddInput:m_impl->audio_input]) {
                m_last_error = "AVAssetWriter cannot add audio input.";
                return AVB_ERROR_OPEN_FAILED;
            }
            [m_impl->writer addInput:m_impl->audio_input];
            m_impl->has_audio = true;
        }

        if (!m_impl->has_video && !m_impl->has_audio) {
            m_last_error = "Encoder requires at least one of video/audio enabled.";
            return AVB_ERROR_INVALID_ARGUMENT;
        }

        if (![m_impl->writer startWriting]) {
            m_last_error = m_impl->writer.error
                ? m_impl->writer.error.localizedDescription.UTF8String
                : "AVAssetWriter startWriting failed.";
            return AVB_ERROR_OPEN_FAILED;
        }
        [m_impl->writer startSessionAtSourceTime:kCMTimeZero];

        return AVB_OK;
    }
}

// Build a standalone IOSurface-backed CVPixelBuffer from an avb_video_frame,
// copying each plane. (Standalone rather than pool-backed so queued frames don't
// exhaust a fixed-size pool.)
static CVPixelBufferRef avb_make_pixel_buffer(const avb_video_frame &f, OSType cvfmt) {
    NSDictionary *attrs = @{ (NSString *)kCVPixelBufferIOSurfacePropertiesKey: @{} };
    CVPixelBufferRef pb = NULL;
    if (CVPixelBufferCreate(kCFAllocatorDefault, f.width, f.height, cvfmt,
                            (__bridge CFDictionaryRef)attrs, &pb) != kCVReturnSuccess) {
        return NULL;
    }
    CVPixelBufferLockBaseAddress(pb, 0);
    size_t planes = CVPixelBufferGetPlaneCount(pb);
    if (planes == 0) {
        uint8_t *dst   = (uint8_t *)CVPixelBufferGetBaseAddress(pb);
        size_t dstrow  = CVPixelBufferGetBytesPerRow(pb);
        size_t copyrow = std::min((size_t)f.plane_stride[0], dstrow);
        for (int y = 0; y < f.height; ++y) {
            memcpy(dst + (size_t)y * dstrow,
                   f.plane_data[0] + (size_t)y * f.plane_stride[0], copyrow);
        }
    } else {
        for (size_t p = 0; p < planes && p < (size_t)f.plane_count; ++p) {
            uint8_t *dst   = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pb, p);
            size_t dstrow  = CVPixelBufferGetBytesPerRowOfPlane(pb, p);
            size_t rows    = CVPixelBufferGetHeightOfPlane(pb, p);
            size_t copyrow = std::min((size_t)f.plane_stride[p], dstrow);
            for (size_t y = 0; y < rows; ++y) {
                memcpy(dst + y * dstrow,
                       f.plane_data[p] + y * (size_t)f.plane_stride[p], copyrow);
            }
        }
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);
    return pb;
}

// Append as many queued samples to each input as it will currently accept. The
// two inputs are serviced independently, so a not-ready input simply leaves its
// data queued instead of blocking (and starving) the other input. Returns false
// only on a hard writer/append failure.
bool AvbEncoderAVFoundation::drain() {
    while (m_impl->video_input && !m_impl->video_q.empty() &&
           m_impl->video_input.readyForMoreMediaData) {
        auto entry = m_impl->video_q.front();
        m_impl->video_q.pop_front();
        BOOL ok = [m_impl->video_adaptor appendPixelBuffer:entry.first
                                      withPresentationTime:entry.second];
        CVPixelBufferRelease(entry.first);
        if (!ok) {
            m_last_error = m_impl->writer.error
                ? m_impl->writer.error.localizedDescription.UTF8String
                : "appendPixelBuffer failed.";
            return false;
        }
    }
    while (m_impl->video_input && !m_impl->custom_video_q.empty() &&
           m_impl->video_input.readyForMoreMediaData) {
        CMSampleBufferRef sb = m_impl->custom_video_q.front();
        m_impl->custom_video_q.pop_front();
        BOOL ok = [m_impl->video_input appendSampleBuffer:sb];
        CFRelease(sb);
        if (!ok) {
            m_last_error = m_impl->writer.error
                ? m_impl->writer.error.localizedDescription.UTF8String
                : "appendSampleBuffer (custom video) failed.";
            return false;
        }
    }
    while (m_impl->audio_input && !m_impl->audio_q.empty() &&
           m_impl->audio_input.readyForMoreMediaData) {
        CMSampleBufferRef sb = m_impl->audio_q.front();
        m_impl->audio_q.pop_front();
        BOOL ok = [m_impl->audio_input appendSampleBuffer:sb];
        CFRelease(sb);
        if (!ok) {
            m_last_error = m_impl->writer.error
                ? m_impl->writer.error.localizedDescription.UTF8String
                : "appendSampleBuffer (audio) failed.";
            return false;
        }
    }
    return true;
}

static CMTime avb_packet_time(int64_t value, double seconds,
                              int tb_num, int tb_den,
                              double fallback_seconds) {
    if (value >= 0 && tb_num > 0 && tb_den > 0) {
        return CMTimeMake(value * (int64_t)tb_num, tb_den);
    }
    double s = seconds >= 0.0 ? seconds : fallback_seconds;
    return CMTimeMakeWithSeconds(s, 90000);
}

avb_result AvbEncoderAVFoundation::write_custom_video_packet(
    avb_encoded_packet &packet,
    double fallback_pts
) {
    if (!packet.data || packet.size <= 0) return AVB_ERROR_INVALID_ARGUMENT;
    if (!m_impl->custom_video_fmt) return AVB_ERROR_INVALID_ARGUMENT;

    @autoreleasepool {
        CMBlockBufferRef block = NULL;
        OSStatus st = CMBlockBufferCreateWithMemoryBlock(
            kCFAllocatorDefault, NULL, (size_t)packet.size, kCFAllocatorDefault,
            NULL, 0, (size_t)packet.size, kCMBlockBufferAssureMemoryNowFlag,
            &block);
        if (st != kCMBlockBufferNoErr || !block) {
            m_last_error = "CMBlockBufferCreate (custom video) failed.";
            return AVB_ERROR_ENCODE_FAILED;
        }
        CMBlockBufferReplaceDataBytes(packet.data, block, 0, (size_t)packet.size);

        double fallback_dur = 1.0 / m_impl->frame_rate;
        CMTime pts = avb_packet_time(packet.pts, packet.pts_sec,
                                     packet.time_base_num, packet.time_base_den,
                                     fallback_pts);
        CMTime dts = avb_packet_time(packet.dts, -1.0,
                                     packet.time_base_num, packet.time_base_den,
                                     CMTimeGetSeconds(pts));
        CMTime dur = packet.duration >= 0 &&
                     packet.time_base_num > 0 &&
                     packet.time_base_den > 0
            ? CMTimeMake(packet.duration * (int64_t)packet.time_base_num,
                         packet.time_base_den)
            : CMTimeMakeWithSeconds(packet.duration_sec > 0.0
                                    ? packet.duration_sec : fallback_dur,
                                    90000);
        CMSampleTimingInfo timing = { dur, pts, dts };
        size_t sample_size = (size_t)packet.size;
        CMSampleBufferRef sb = NULL;
        st = CMSampleBufferCreateReady(kCFAllocatorDefault, block,
                                       m_impl->custom_video_fmt, 1, 1, &timing,
                                       1, &sample_size, &sb);
        CFRelease(block);
        if (st != noErr || !sb) {
            m_last_error = "CMSampleBufferCreateReady (custom video) failed.";
            return AVB_ERROR_ENCODE_FAILED;
        }

        if (!packet.keyframe) {
            CFArrayRef attachments =
                CMSampleBufferGetSampleAttachmentsArray(sb, true);
            if (attachments && CFArrayGetCount(attachments) > 0) {
                CFMutableDictionaryRef dict =
                    (CFMutableDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
                CFDictionarySetValue(dict, kCMSampleAttachmentKey_NotSync,
                                     kCFBooleanTrue);
            }
        }

        m_impl->custom_video_q.push_back(sb);
        if (!drain()) return AVB_ERROR_ENCODE_FAILED;
        return AVB_OK;
    }
}

avb_result AvbEncoderAVFoundation::write_video(const avb_video_frame &frame, double pts_sec) {
    if (!m_impl->has_video) return AVB_ERROR_INVALID_ARGUMENT;
    if (m_impl->custom_video) {
        avb_encoded_packet packet{};
        avb_result res = m_impl->custom_video_encoder->encode_frame(
            m_impl->custom_video_ctx, &frame, pts_sec, &packet);
        if (res != AVB_OK) return res;
        double fallback_pts = pts_sec >= 0.0 ? pts_sec
                           : frame.pts_sec >= 0.0 ? frame.pts_sec
                           : (double)m_impl->video_frame_index / m_impl->frame_rate;
        res = write_custom_video_packet(packet, fallback_pts);
        if (m_impl->custom_video_encoder->release_packet)
            m_impl->custom_video_encoder->release_packet(m_impl->custom_video_ctx, &packet);
        if (res == AVB_OK) m_impl->video_frame_index++;
        return res;
    }
    if (frame.format != m_impl->input_format) {
        m_last_error = "Frame pixel format does not match configured input_format.";
        return AVB_ERROR_INVALID_ARGUMENT;
    }

    @autoreleasepool {
        // PTS resolution order: explicit arg, then the frame's own pts (so
        // decoded frames pass straight through), then derived from frame_rate.
        double pts = pts_sec >= 0.0      ? pts_sec
                   : frame.pts_sec >= 0.0 ? frame.pts_sec
                   : (double)m_impl->video_frame_index / m_impl->frame_rate;
        CMTime t = CMTimeMakeWithSeconds(pts, 90000);

        CVPixelBufferRef pb = avb_make_pixel_buffer(frame, m_impl->cv_pixel_format);
        if (!pb) {
            m_last_error = "CVPixelBuffer creation failed.";
            return AVB_ERROR_ENCODE_FAILED;
        }
        m_impl->video_q.emplace_back(pb, t);
        m_impl->video_frame_index++;

        if (!drain()) return AVB_ERROR_ENCODE_FAILED;
        return AVB_OK;
    }
}

avb_result AvbEncoderAVFoundation::write_audio_f32(const float *src_interleaved, int frames) {
    if (!m_impl->has_audio) return AVB_ERROR_INVALID_ARGUMENT;

    @autoreleasepool {
        size_t data_size = (size_t)frames * m_impl->channels * sizeof(float);

        CMBlockBufferRef block = NULL;
        OSStatus st = CMBlockBufferCreateWithMemoryBlock(
            kCFAllocatorDefault, NULL, data_size, kCFAllocatorDefault, NULL,
            0, data_size, kCMBlockBufferAssureMemoryNowFlag, &block);
        if (st != kCMBlockBufferNoErr || !block) {
            m_last_error = "CMBlockBufferCreate failed.";
            return AVB_ERROR_ENCODE_FAILED;
        }
        CMBlockBufferReplaceDataBytes(src_interleaved, block, 0, data_size);

        CMTime pts = CMTimeMake(m_impl->audio_sample_count, m_impl->sample_rate);
        CMSampleTimingInfo timing = { CMTimeMake(1, m_impl->sample_rate), pts, kCMTimeInvalid };
        size_t sample_size = (size_t)m_impl->channels * sizeof(float);

        CMSampleBufferRef sb = NULL;
        st = CMSampleBufferCreate(kCFAllocatorDefault, block, true, NULL, NULL,
                                  m_impl->audio_fmt, frames, 1, &timing,
                                  1, &sample_size, &sb);
        CFRelease(block);
        if (st != noErr || !sb) {
            m_last_error = "CMSampleBufferCreate (audio) failed.";
            return AVB_ERROR_ENCODE_FAILED;
        }
        m_impl->audio_q.push_back(sb);
        m_impl->audio_sample_count += frames;

        if (!drain()) return AVB_ERROR_ENCODE_FAILED;
        return AVB_OK;
    }
}

avb_result AvbEncoderAVFoundation::finish() {
    if (!m_impl->writer) return AVB_ERROR_INVALID_ARGUMENT;
    if (m_impl->finished) return AVB_OK;

    @autoreleasepool {
        if (m_impl->custom_video && m_impl->custom_video_encoder->flush) {
            while (true) {
                avb_encoded_packet packet{};
                avb_result r = m_impl->custom_video_encoder->flush(
                    m_impl->custom_video_ctx, &packet);
                if (r == AVB_ERROR_EOF || r == AVB_ERROR_AGAIN) break;
                if (r != AVB_OK) return r;
                r = write_custom_video_packet(
                    packet,
                    (double)m_impl->video_frame_index / m_impl->frame_rate);
                if (m_impl->custom_video_encoder->release_packet)
                    m_impl->custom_video_encoder->release_packet(
                        m_impl->custom_video_ctx, &packet);
                if (r != AVB_OK) return r;
                m_impl->video_frame_index++;
            }
        }

        // Drain queued samples. Once a track queue becomes empty, mark that
        // input finished immediately so the writer can stop waiting for more
        // samples on that track while the other track drains.
        bool video_marked = false;
        bool audio_marked = false;
        int idle = 0;
        while (!video_marked || !audio_marked) {
            if (m_impl->writer.status != AVAssetWriterStatusWriting) break;

            if (!video_marked && !m_impl->video_input) video_marked = true;
            if (!audio_marked && !m_impl->audio_input) audio_marked = true;

            if (!video_marked &&
                m_impl->video_q.empty() &&
                m_impl->custom_video_q.empty()) {
                [m_impl->video_input markAsFinished];
                video_marked = true;
            }
            if (!audio_marked && m_impl->audio_q.empty()) {
                [m_impl->audio_input markAsFinished];
                audio_marked = true;
            }
            if (video_marked && audio_marked) break;

            size_t before =
                m_impl->video_q.size() +
                m_impl->custom_video_q.size() +
                m_impl->audio_q.size();
            if (!drain()) return AVB_ERROR_ENCODE_FAILED;
            size_t after =
                m_impl->video_q.size() +
                m_impl->custom_video_q.size() +
                m_impl->audio_q.size();
            if (after == before) {
                if (++idle > 10000) {
                    m_last_error = "Timed out draining encoder queues.";
                    return AVB_ERROR_ENCODE_FAILED;
                }
                usleep(1000);
            } else {
                idle = 0;
            }
        }

        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [m_impl->writer finishWritingWithCompletionHandler:^{
            dispatch_semaphore_signal(sem);
        }];
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
        m_impl->finished = true;

        if (m_impl->writer.status != AVAssetWriterStatusCompleted) {
            m_last_error = m_impl->writer.error
                ? m_impl->writer.error.localizedDescription.UTF8String
                : "finishWriting did not complete.";
            return AVB_ERROR_ENCODE_FAILED;
        }
        return AVB_OK;
    }
}

#else // !__APPLE__

AvbEncoderAVFoundation::AvbEncoderAVFoundation() { m_impl = nullptr; }
AvbEncoderAVFoundation::~AvbEncoderAVFoundation() {}
avb_result AvbEncoderAVFoundation::open(const char *, const avb_encode_options &) {
    return AVB_ERROR_BACKEND_NOT_AVAILABLE;
}
avb_result AvbEncoderAVFoundation::write_video(const avb_video_frame &, double) {
    return AVB_ERROR_BACKEND_NOT_AVAILABLE;
}
avb_result AvbEncoderAVFoundation::write_audio_f32(const float *, int) {
    return AVB_ERROR_BACKEND_NOT_AVAILABLE;
}
avb_result AvbEncoderAVFoundation::finish() { return AVB_ERROR_BACKEND_NOT_AVAILABLE; }
const char *AvbEncoderAVFoundation::get_last_error() const {
    return m_last_error.empty() ? nullptr : m_last_error.c_str();
}

#endif // __APPLE__
