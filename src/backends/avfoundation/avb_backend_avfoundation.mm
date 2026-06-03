#include "avb_backend_avfoundation.hh"

#ifdef __APPLE__

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <cstring>
#include <vector>

// Synchronously load the tracks of a given media type. AVFoundation deprecated
// the synchronous -tracksWithMediaType: accessor in macOS 15 in favour of the
// async loadTracksWithMediaType:completionHandler:; since this backend exposes a
// synchronous C API we bridge back to a blocking call with a semaphore. The
// completion handler is delivered on an internal AVFoundation queue (never the
// calling thread), so waiting here does not deadlock.
static NSArray<AVAssetTrack *> *avb_load_tracks(AVAsset *asset, AVMediaType type) {
    __block NSArray<AVAssetTrack *> *result = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [asset loadTracksWithMediaType:type
                 completionHandler:^(NSArray<AVAssetTrack *> *tracks, NSError *error) {
        if (!error) result = tracks;
        dispatch_semaphore_signal(sem);
    }];
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    return result;
}

// Map a CoreMedia media-subtype FourCC to the same codec name FFmpeg reports, so
// avb_media_info::codec_name means the *source* codec consistently across
// backends. Unknown codes fall back to their printable FourCC.
static std::string avb_fourcc_to_name(FourCharCode code) {
    switch (code) {
        // Video
        case kCMVideoCodecType_H264:           return "h264";
        case kCMVideoCodecType_HEVC:           return "hevc";
        case kCMVideoCodecType_MPEG4Video:     return "mpeg4";
        case kCMVideoCodecType_MPEG2Video:     return "mpeg2video";
        case kCMVideoCodecType_MPEG1Video:     return "mpeg1video";
        case kCMVideoCodecType_JPEG:           return "mjpeg";
        case kCMVideoCodecType_AppleProRes4444:
        case kCMVideoCodecType_AppleProRes422:
        case kCMVideoCodecType_AppleProRes422HQ:
        case kCMVideoCodecType_AppleProRes422LT:
        case kCMVideoCodecType_AppleProRes422Proxy: return "prores";
        case kCMVideoCodecType_VP9:            return "vp9";
        case 'av01':                           return "av1";
        // Audio
        case kAudioFormatMPEG4AAC:             return "aac";
        case kAudioFormatMPEGLayer3:           return "mp3";
        case kAudioFormatMPEGLayer2:           return "mp2";
        case kAudioFormatMPEGLayer1:           return "mp1";
        case kAudioFormatLinearPCM:            return "pcm";
        case kAudioFormatAppleLossless:        return "alac";
        case kAudioFormatAC3:                  return "ac3";
        case kAudioFormatEnhancedAC3:          return "eac3";
        case kAudioFormatOpus:                 return "opus";
        case kAudioFormatFLAC:                 return "flac";
        case kAudioFormatAMR:                  return "amr_nb";
        default: break;
    }
    // Printable FourCC fallback, e.g. 'abcd'. Non-printable bytes become '?'.
    char c[5] = {
        (char)((code >> 24) & 0xff), (char)((code >> 16) & 0xff),
        (char)((code >> 8) & 0xff),  (char)(code & 0xff), 0
    };
    for (int i = 0; i < 4; ++i) {
        if (c[i] < 0x20 || c[i] > 0x7e) c[i] = '?';
    }
    return std::string(c);
}

// Resolve a requested avb_pixel_format to a CoreVideo pixel format type.
// AVAssetReader does not reliably emit 32RGBA, so RGBA8 is requested as BGRA8
// here and byte-swizzled to RGBA after the copy (see open_file/read_video_frame).
static OSType avb_cvpixfmt(avb_pixel_format want) {
    switch (want) {
        case AVB_PIXEL_FORMAT_NV12:
            return kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        case AVB_PIXEL_FORMAT_I420:
            return kCVPixelFormatType_420YpCbCr8Planar;
        case AVB_PIXEL_FORMAT_BGRA8:
        case AVB_PIXEL_FORMAT_RGBA8:
        case AVB_PIXEL_FORMAT_UNKNOWN:
        default:
            return kCVPixelFormatType_32BGRA;
    }
}

// Build the AVAssetReaderTrackOutput settings for interleaved float32 PCM.
// req_rate/req_channels of 0 leave the source value untouched; otherwise
// AVFoundation resamples / remixes to the requested values.
static NSDictionary *avb_audio_settings(int req_rate, int req_channels) {
    NSMutableDictionary *s = [@{
        AVFormatIDKey:               @(kAudioFormatLinearPCM),
        AVLinearPCMBitDepthKey:      @32,
        AVLinearPCMIsFloatKey:       @YES,
        AVLinearPCMIsNonInterleaved: @NO,
    } mutableCopy];
    if (req_rate > 0)     s[AVSampleRateKey]       = @(req_rate);
    if (req_channels > 0) s[AVNumberOfChannelsKey] = @(req_channels);
    return s;
}

// Source-codec FourCC of a track's first format description, or 0 if unavailable.
static FourCharCode avb_track_codec(AVAssetTrack *track) {
    NSArray *formats = track.formatDescriptions;
    if (formats.count == 0) return 0;
    CMFormatDescriptionRef fmt = (__bridge CMFormatDescriptionRef)formats[0];
    return CMFormatDescriptionGetMediaSubType(fmt);
}

struct AvbBackendAVFoundation::Impl {
    AVAsset *asset = nil;
    AVAssetReader *reader = nil;
    AVAssetReaderTrackOutput *audio_output = nil;
    AVAssetReaderTrackOutput *video_output = nil;

    int audio_stream_idx = -1;
    int video_stream_idx = -1;

    int sample_rate  = 0;   // effective output rate
    int channels     = 0;   // effective output channel count
    int req_sample_rate = 0; // requested override (0 = source)
    int req_channels    = 0; // requested override (0 = source)
    int width        = 0;
    int height       = 0;
    double duration_sec  = 0.0;
    double frame_rate    = 0.0;

    OSType cv_pixel_format          = kCVPixelFormatType_32BGRA;
    avb_pixel_format video_avb_fmt  = AVB_PIXEL_FORMAT_BGRA8;
    bool swizzle_rgba               = false; // request BGRA, emit RGBA

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

avb_result AvbBackendAVFoundation::open_file(const char *path, const avb_decode_options &options) {
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

        // Audio track. options.audio_stream_index selects the Nth audio track
        // (0-based); -1 picks the first. (Stream indices are necessarily
        // backend-specific; AVFoundation has no file-wide stream numbering.)
        if (options.enable_audio) {
            NSArray<AVAssetTrack *> *audio_tracks =
                avb_load_tracks(m_impl->asset, AVMediaTypeAudio);
            int idx = options.audio_stream_index >= 0 ? options.audio_stream_index : 0;
            if (idx < (int)audio_tracks.count) {
                AVAssetTrack *track = audio_tracks[idx];

                m_impl->req_sample_rate = options.audio_sample_rate;
                m_impl->req_channels    = options.audio_channels;

                m_impl->audio_output = [AVAssetReaderTrackOutput
                    assetReaderTrackOutputWithTrack:track
                    outputSettings:avb_audio_settings(m_impl->req_sample_rate,
                                                      m_impl->req_channels)];
                m_impl->audio_output.alwaysCopiesSampleData = NO;
                [m_impl->reader addOutput:m_impl->audio_output];
                m_impl->audio_stream_idx = idx;

                // Effective output format: requested override, else source value.
                int src_rate = 0, src_ch = 0;
                NSArray *formats = track.formatDescriptions;
                if (formats.count > 0) {
                    CMAudioFormatDescriptionRef fmt =
                        (__bridge CMAudioFormatDescriptionRef)formats[0];
                    const AudioStreamBasicDescription *asbd =
                        CMAudioFormatDescriptionGetStreamBasicDescription(fmt);
                    if (asbd) {
                        src_rate = (int)asbd->mSampleRate;
                        src_ch   = (int)asbd->mChannelsPerFrame;
                    }
                }
                m_impl->sample_rate = m_impl->req_sample_rate > 0 ? m_impl->req_sample_rate : src_rate;
                m_impl->channels    = m_impl->req_channels    > 0 ? m_impl->req_channels    : src_ch;
                m_impl->audio_codec_name = avb_fourcc_to_name(avb_track_codec(track));
            }
        }

        // Video track (see audio note above for stream-index semantics).
        if (options.enable_video) {
            NSArray<AVAssetTrack *> *video_tracks =
                avb_load_tracks(m_impl->asset, AVMediaTypeVideo);
            int idx = options.video_stream_index >= 0 ? options.video_stream_index : 0;
            if (idx < (int)video_tracks.count) {
                AVAssetTrack *track = video_tracks[idx];

                m_impl->cv_pixel_format = avb_cvpixfmt(options.video_format);
                m_impl->video_avb_fmt =
                    options.video_format == AVB_PIXEL_FORMAT_UNKNOWN
                        ? AVB_PIXEL_FORMAT_BGRA8 : options.video_format;
                m_impl->swizzle_rgba =
                    (options.video_format == AVB_PIXEL_FORMAT_RGBA8);
                NSDictionary *settings = @{
                    (NSString *)kCVPixelBufferPixelFormatTypeKey:
                        @(m_impl->cv_pixel_format),
                };

                m_impl->video_output = [AVAssetReaderTrackOutput
                    assetReaderTrackOutputWithTrack:track
                    outputSettings:settings];
                m_impl->video_output.alwaysCopiesSampleData = NO;
                [m_impl->reader addOutput:m_impl->video_output];
                m_impl->video_stream_idx = idx;

                CGSize size = track.naturalSize;
                m_impl->width  = (int)size.width;
                m_impl->height = (int)size.height;
                m_impl->frame_rate   = track.nominalFrameRate;
                m_impl->video_codec_name = avb_fourcc_to_name(avb_track_codec(track));
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
        out_info.audio.stream_index = m_impl->audio_stream_idx;
        out_info.audio.sample_rate  = m_impl->sample_rate;
        out_info.audio.channels     = m_impl->channels;
        out_info.audio.duration_sec = m_impl->duration_sec;
        out_info.audio.codec_name   = m_impl->audio_codec_name.c_str();
    }

    if (m_impl->video_output) {
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
        if (CMTimeCompare(dur, kCMTimeZero) < 0) dur = kCMTimeZero;
        CMTimeRange range = CMTimeRangeMake(start, dur);
        m_impl->reader.timeRange = range;

        if (m_impl->audio_output) {
            AVAssetTrack *track =
                avb_load_tracks(m_impl->asset, AVMediaTypeAudio)[m_impl->audio_stream_idx];
            m_impl->audio_output = [AVAssetReaderTrackOutput
                assetReaderTrackOutputWithTrack:track
                outputSettings:avb_audio_settings(m_impl->req_sample_rate,
                                                  m_impl->req_channels)];
            m_impl->audio_output.alwaysCopiesSampleData = NO;
            [m_impl->reader addOutput:m_impl->audio_output];
        }

        if (m_impl->video_output) {
            AVAssetTrack *track =
                avb_load_tracks(m_impl->asset, AVMediaTypeVideo)[m_impl->video_stream_idx];
            NSDictionary *settings = @{
                (NSString *)kCVPixelBufferPixelFormatTypeKey: @(m_impl->cv_pixel_format),
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

        size_t width  = CVPixelBufferGetWidth(image);
        size_t height = CVPixelBufferGetHeight(image);

        // CVPixelBuffer may be packed (plane count 0) or planar (e.g. NV12 has
        // two planes). Gather each plane's source pointer/stride/rows, then copy
        // them contiguously into one buffer so the caller gets stable pointers.
        const void *src_base[AVB_MAX_PLANES] = {0};
        size_t      src_stride[AVB_MAX_PLANES] = {0};
        size_t      src_rows[AVB_MAX_PLANES]   = {0};

        size_t plane_count = CVPixelBufferGetPlaneCount(image);
        if (plane_count == 0) {
            // Packed format: treat the whole image as a single plane.
            plane_count   = 1;
            src_base[0]   = CVPixelBufferGetBaseAddress(image);
            src_stride[0] = CVPixelBufferGetBytesPerRow(image);
            src_rows[0]   = height;
        } else {
            if (plane_count > AVB_MAX_PLANES) plane_count = AVB_MAX_PLANES;
            for (size_t p = 0; p < plane_count; ++p) {
                src_base[p]   = CVPixelBufferGetBaseAddressOfPlane(image, p);
                src_stride[p] = CVPixelBufferGetBytesPerRowOfPlane(image, p);
                src_rows[p]   = CVPixelBufferGetHeightOfPlane(image, p);
            }
        }

        size_t plane_size[AVB_MAX_PLANES] = {0};
        size_t total = 0;
        for (size_t p = 0; p < plane_count; ++p) {
            plane_size[p] = src_stride[p] * src_rows[p];
            total += plane_size[p];
        }

        m_impl->video_frame_buf.resize(total);
        unsigned char *dst = m_impl->video_frame_buf.data();

        out_frame = {};
        out_frame.width       = (int)width;
        out_frame.height      = (int)height;
        out_frame.format      = m_impl->video_avb_fmt;
        out_frame.pts_sec     = CMTimeGetSeconds(pts);
        out_frame.plane_count = (int)plane_count;
        out_frame.data_size   = (int)total;

        size_t offset = 0;
        for (size_t p = 0; p < plane_count; ++p) {
            memcpy(dst + offset, src_base[p], plane_size[p]);
            out_frame.plane_data[p]   = dst + offset;
            out_frame.plane_stride[p] = (int)src_stride[p];
            offset += plane_size[p];
        }

        // AVAssetReader gave us BGRA; convert to RGBA in place if requested.
        if (m_impl->swizzle_rgba) {
            for (int y = 0; y < out_frame.height; ++y) {
                unsigned char *row = dst + (size_t)y * out_frame.plane_stride[0];
                for (int x = 0; x < out_frame.width; ++x) {
                    unsigned char *px = row + x * 4;
                    unsigned char b = px[0];
                    px[0] = px[2]; // R
                    px[2] = b;     // B
                }
            }
        }

        // Plane-0 aliases for packed-format consumers.
        out_frame.data   = out_frame.plane_data[0];
        out_frame.stride = out_frame.plane_stride[0];

        CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
        CFRelease(sample_buf);
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
avb_result AvbBackendAVFoundation::open_file(const char *, const avb_decode_options &) {
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
