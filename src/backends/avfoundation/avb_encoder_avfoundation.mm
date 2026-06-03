#include "avb_encoder_avfoundation.hh"

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

    bool   has_video = false;
    bool   has_audio = false;
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
    std::deque<CMSampleBufferRef>                   audio_q;

    ~Impl() {
        for (auto &v : video_q) CVPixelBufferRelease(v.first);
        for (auto sb : audio_q) CFRelease(sb);
        if (audio_fmt) CFRelease(audio_fmt);
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

avb_result AvbEncoderAVFoundation::open(const char *path, const avb_encode_options &options) {
    @autoreleasepool {
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
            bool fmt_ok = true;
            m_impl->cv_pixel_format = avb_enc_cvpixfmt(options.video.input_format, &fmt_ok);
            if (!fmt_ok) {
                m_last_error = "Unsupported video input_format (use BGRA8 or NV12).";
                return AVB_ERROR_INVALID_ARGUMENT;
            }
            m_impl->input_format = options.video.input_format == AVB_PIXEL_FORMAT_UNKNOWN
                ? AVB_PIXEL_FORMAT_BGRA8 : options.video.input_format;
            m_impl->width      = options.video.width;
            m_impl->height     = options.video.height;
            m_impl->frame_rate = options.video.frame_rate > 0 ? options.video.frame_rate : 30.0;

            NSMutableDictionary *vsettings = [@{
                AVVideoCodecKey:  AVVideoCodecTypeH264,
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

avb_result AvbEncoderAVFoundation::write_video(const avb_video_frame &frame, double pts_sec) {
    if (!m_impl->has_video) return AVB_ERROR_INVALID_ARGUMENT;
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
        // Drain any remaining queued samples. Both inputs are serviced each pass;
        // sleep only when neither can currently accept data.
        int idle = 0;
        while (!m_impl->video_q.empty() || !m_impl->audio_q.empty()) {
            if (m_impl->writer.status != AVAssetWriterStatusWriting) break;
            size_t before = m_impl->video_q.size() + m_impl->audio_q.size();
            if (!drain()) return AVB_ERROR_ENCODE_FAILED;
            size_t after = m_impl->video_q.size() + m_impl->audio_q.size();
            if (after == before) {
                if (++idle > 10000) { // ~10s with no progress
                    m_last_error = "Timed out draining encoder queues.";
                    return AVB_ERROR_ENCODE_FAILED;
                }
                usleep(1000);
            } else {
                idle = 0;
            }
        }

        if (m_impl->video_input) [m_impl->video_input markAsFinished];
        if (m_impl->audio_input) [m_impl->audio_input markAsFinished];

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
