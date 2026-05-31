#include "avb_backend_avfoundation.hh"

#ifdef __APPLE__

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <cstring>
#include <vector>

struct AvbBackendAVFoundation::Impl {
    AVAsset *asset = nil;
    AVAssetReader *reader = nil;
    AVAssetReaderTrackOutput *audio_output = nil;
    AVAssetReaderTrackOutput *video_output = nil;

    int sample_rate  = 0;
    int channels     = 0;
    int width        = 0;
    int height       = 0;
    double duration_sec  = 0.0;
    double frame_rate    = 0.0;

    std::string audio_codec_name;
    std::string video_codec_name;

    std::vector<float> audio_buf;
    int audio_buf_pos = 0;

    std::vector<unsigned char> video_frame_buf;
};

AvbBackendAVFoundation::AvbBackendAVFoundation() {
    m_impl = new Impl();
}

AvbBackendAVFoundation::~AvbBackendAVFoundation() {
    if (m_impl) {
        if (m_impl->reader) [m_impl->reader cancelReading];
        delete m_impl;
    }
}

const char *AvbBackendAVFoundation::get_backend_name() const { return "avfoundation"; }
const char *AvbBackendAVFoundation::get_last_error() const {
    return m_last_error.empty() ? nullptr : m_last_error.c_str();
}

avb_result AvbBackendAVFoundation::open_file(const char *path, const avb_open_options &options) {
    @autoreleasepool {
        NSString *ns_path = [NSString stringWithUTF8String:path];
        NSURL *url = [NSURL fileURLWithPath:ns_path];

        m_impl->asset = [AVAsset assetWithURL:url];
        if (!m_impl->asset) {
            m_last_error = "AVAsset creation failed.";
            return AVB_ERROR_OPEN_FAILED;
        }

        NSError *error = nil;
        m_impl->reader = [AVAssetReader assetReaderWithAsset:m_impl->asset error:&error];
        if (!m_impl->reader || error) {
            m_last_error = "AVAssetReader creation failed.";
            return AVB_ERROR_OPEN_FAILED;
        }

        // Duration
        CMTime duration = m_impl->asset.duration;
        m_impl->duration_sec = CMTimeGetSeconds(duration);

        // Audio track
        if (options.enable_audio) {
            NSArray<AVAssetTrack *> *audio_tracks =
                [m_impl->asset tracksWithMediaType:AVMediaTypeAudio];
            if (audio_tracks.count > 0) {
                AVAssetTrack *track = audio_tracks[0];

                NSDictionary *settings = @{
                    AVFormatIDKey: @(kAudioFormatLinearPCM),
                    AVLinearPCMBitDepthKey: @32,
                    AVLinearPCMIsFloatKey: @YES,
                    AVLinearPCMIsNonInterleaved: @NO,
                };

                m_impl->audio_output = [AVAssetReaderTrackOutput
                    assetReaderTrackOutputWithTrack:track
                    outputSettings:settings];
                m_impl->audio_output.alwaysCopiesSampleData = NO;
                [m_impl->reader addOutput:m_impl->audio_output];

                // Get format info
                NSArray *formats = track.formatDescriptions;
                if (formats.count > 0) {
                    CMAudioFormatDescriptionRef fmt =
                        (__bridge CMAudioFormatDescriptionRef)formats[0];
                    const AudioStreamBasicDescription *asbd =
                        CMAudioFormatDescriptionGetStreamBasicDescription(fmt);
                    if (asbd) {
                        m_impl->sample_rate = (int)asbd->mSampleRate;
                        m_impl->channels    = (int)asbd->mChannelsPerFrame;
                    }
                }
                m_impl->audio_codec_name = "pcm_f32";
            }
        }

        // Video track
        if (options.enable_video) {
            NSArray<AVAssetTrack *> *video_tracks =
                [m_impl->asset tracksWithMediaType:AVMediaTypeVideo];
            if (video_tracks.count > 0) {
                AVAssetTrack *track = video_tracks[0];

                NSDictionary *settings = @{
                    (NSString *)kCVPixelBufferPixelFormatTypeKey:
                        @(kCVPixelFormatType_32BGRA),
                };

                m_impl->video_output = [AVAssetReaderTrackOutput
                    assetReaderTrackOutputWithTrack:track
                    outputSettings:settings];
                m_impl->video_output.alwaysCopiesSampleData = NO;
                [m_impl->reader addOutput:m_impl->video_output];

                CGSize size = track.naturalSize;
                m_impl->width  = (int)size.width;
                m_impl->height = (int)size.height;
                m_impl->frame_rate   = track.nominalFrameRate;
                m_impl->video_codec_name = "bgra";
            }
        }

        if (!m_impl->audio_output && !m_impl->video_output) {
            m_last_error = "No supported audio or video stream found.";
            return AVB_ERROR_STREAM_NOT_FOUND;
        }

        if (![m_impl->reader startReading]) {
            m_last_error = "AVAssetReader startReading failed.";
            return AVB_ERROR_OPEN_FAILED;
        }

        return AVB_OK;
    }
}

avb_result AvbBackendAVFoundation::get_media_info(avb_media_info &out_info) {
    if (!m_impl->reader) return AVB_ERROR_INVALID_ARGUMENT;

    out_info = {};
    out_info.backend_name  = "avfoundation";
    out_info.duration_sec  = m_impl->duration_sec;

    if (m_impl->audio_output) {
        out_info.audio.available    = 1;
        out_info.audio.stream_index = 0;
        out_info.audio.sample_rate  = m_impl->sample_rate;
        out_info.audio.channels     = m_impl->channels;
        out_info.audio.duration_sec = m_impl->duration_sec;
        out_info.audio.codec_name   = m_impl->audio_codec_name.c_str();
    }

    if (m_impl->video_output) {
        out_info.video.available    = 1;
        out_info.video.stream_index = 0;
        out_info.video.width        = m_impl->width;
        out_info.video.height       = m_impl->height;
        out_info.video.frame_rate   = m_impl->frame_rate;
        out_info.video.duration_sec = m_impl->duration_sec;
        out_info.video.codec_name   = m_impl->video_codec_name.c_str();
    }

    return AVB_OK;
}

avb_result AvbBackendAVFoundation::seek(double seconds) {
    if (!m_impl->reader) return AVB_ERROR_INVALID_ARGUMENT;

    // AVAssetReader does not support seeking directly — recreate it with a time range.
    @autoreleasepool {
        [m_impl->reader cancelReading];

        NSError *error = nil;
        m_impl->reader = [AVAssetReader assetReaderWithAsset:m_impl->asset error:&error];
        if (!m_impl->reader || error) {
            m_last_error = "AVAssetReader recreation failed during seek.";
            return AVB_ERROR_SEEK_FAILED;
        }

        CMTime start = CMTimeMakeWithSeconds(seconds, 600);
        CMTime dur   = CMTimeSubtract(m_impl->asset.duration, start);
        if (CMTIME_IS_NEGATIVE(dur)) dur = kCMTimeZero;
        CMTimeRange range = CMTimeRangeMake(start, dur);
        m_impl->reader.timeRange = range;

        if (m_impl->audio_output) {
            AVAssetTrack *track =
                [m_impl->asset tracksWithMediaType:AVMediaTypeAudio][0];
            NSDictionary *settings = @{
                AVFormatIDKey: @(kAudioFormatLinearPCM),
                AVLinearPCMBitDepthKey: @32,
                AVLinearPCMIsFloatKey: @YES,
                AVLinearPCMIsNonInterleaved: @NO,
            };
            m_impl->audio_output = [AVAssetReaderTrackOutput
                assetReaderTrackOutputWithTrack:track outputSettings:settings];
            m_impl->audio_output.alwaysCopiesSampleData = NO;
            [m_impl->reader addOutput:m_impl->audio_output];
        }

        if (m_impl->video_output) {
            AVAssetTrack *track =
                [m_impl->asset tracksWithMediaType:AVMediaTypeVideo][0];
            NSDictionary *settings = @{
                (NSString *)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
            };
            m_impl->video_output = [AVAssetReaderTrackOutput
                assetReaderTrackOutputWithTrack:track outputSettings:settings];
            m_impl->video_output.alwaysCopiesSampleData = NO;
            [m_impl->reader addOutput:m_impl->video_output];
        }

        if (![m_impl->reader startReading]) {
            m_last_error = "AVAssetReader startReading failed after seek.";
            return AVB_ERROR_SEEK_FAILED;
        }

        m_impl->audio_buf.clear();
        m_impl->audio_buf_pos = 0;
    }

    return AVB_OK;
}

int AvbBackendAVFoundation::read_audio_f32(float *dst_interleaved, int frames) {
    if (!m_impl->audio_output) return 0;

    int nb_channels    = m_impl->channels;
    int samples_needed = frames * nb_channels;
    int samples_written = 0;

    while (samples_written < samples_needed) {
        int available = (int)m_impl->audio_buf.size() - m_impl->audio_buf_pos;
        if (available > 0) {
            int to_copy = samples_needed - samples_written;
            if (to_copy > available) to_copy = available;
            memcpy(dst_interleaved + samples_written,
                   m_impl->audio_buf.data() + m_impl->audio_buf_pos,
                   to_copy * sizeof(float));
            m_impl->audio_buf_pos += to_copy;
            samples_written += to_copy;
            if (m_impl->audio_buf_pos >= (int)m_impl->audio_buf.size()) {
                m_impl->audio_buf.clear();
                m_impl->audio_buf_pos = 0;
            }
            continue;
        }

        @autoreleasepool {
            CMSampleBufferRef sample_buf =
                [m_impl->audio_output copyNextSampleBuffer];
            if (!sample_buf) break;

            CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample_buf);
            size_t len = CMBlockBufferGetDataLength(block);
            size_t n_floats = len / sizeof(float);
            m_impl->audio_buf.resize(n_floats);
            CMBlockBufferCopyDataBytes(block, 0, len, m_impl->audio_buf.data());
            m_impl->audio_buf_pos = 0;

            CFRelease(sample_buf);
        }
    }

    return samples_written / nb_channels;
}

avb_result AvbBackendAVFoundation::read_video_frame(avb_video_frame &out_frame) {
    if (!m_impl->video_output) return AVB_ERROR_STREAM_NOT_FOUND;

    @autoreleasepool {
        CMSampleBufferRef sample_buf =
            [m_impl->video_output copyNextSampleBuffer];
        if (!sample_buf) return AVB_ERROR_EOF;

        CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample_buf);

        CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample_buf);
        if (!image) {
            CFRelease(sample_buf);
            return AVB_ERROR_DECODE_FAILED;
        }

        CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
        void *base = CVPixelBufferGetBaseAddress(image);
        size_t width  = CVPixelBufferGetWidth(image);
        size_t height = CVPixelBufferGetHeight(image);
        size_t stride = CVPixelBufferGetBytesPerRow(image);
        size_t len    = stride * height;

        m_impl->video_frame_buf.resize(len);
        memcpy(m_impl->video_frame_buf.data(), base, len);

        CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
        CFRelease(sample_buf);

        out_frame.width     = (int)width;
        out_frame.height    = (int)height;
        out_frame.format    = AVB_PIXEL_FORMAT_BGRA8;
        out_frame.stride    = (int)stride;
        out_frame.pts_sec   = CMTimeGetSeconds(pts);
        out_frame.data      = m_impl->video_frame_buf.data();
        out_frame.data_size = (int)len;
    }

    return AVB_OK;
}

void AvbBackendAVFoundation::release_video_frame(avb_video_frame &frame) {
    memset(&frame, 0, sizeof(frame));
}

#else // !__APPLE__

AvbBackendAVFoundation::AvbBackendAVFoundation() {
    m_impl = new Impl();
    m_last_error = "AVFoundation backend is only available on Apple platforms.";
}
AvbBackendAVFoundation::~AvbBackendAVFoundation() { delete m_impl; }
const char *AvbBackendAVFoundation::get_backend_name() const { return "avfoundation"; }
const char *AvbBackendAVFoundation::get_last_error() const {
    return m_last_error.empty() ? nullptr : m_last_error.c_str();
}
avb_result AvbBackendAVFoundation::open_file(const char *, const avb_open_options &) {
    return AVB_ERROR_BACKEND_NOT_AVAILABLE;
}
avb_result AvbBackendAVFoundation::get_media_info(avb_media_info &) {
    return AVB_ERROR_BACKEND_NOT_AVAILABLE;
}
avb_result AvbBackendAVFoundation::seek(double) { return AVB_ERROR_BACKEND_NOT_AVAILABLE; }
int AvbBackendAVFoundation::read_audio_f32(float *, int) { return 0; }
avb_result AvbBackendAVFoundation::read_video_frame(avb_video_frame &) {
    return AVB_ERROR_BACKEND_NOT_AVAILABLE;
}
void AvbBackendAVFoundation::release_video_frame(avb_video_frame &) {}

#endif // __APPLE__
