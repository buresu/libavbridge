#include "avb_decoder_avfoundation.hh"
#include "../../avb_video_codec_registry.hpp"

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

static uint32_t avb_bswap32(uint32_t v) {
    return ((v & 0x000000ffu) << 24) |
           ((v & 0x0000ff00u) << 8)  |
           ((v & 0x00ff0000u) >> 8)  |
           ((v & 0xff000000u) >> 24);
}

struct AvbDecoderAVFoundation::Impl {
    AVAsset *asset = nil;
    AVAssetReader *reader = nil;
    AVAssetReaderTrackOutput *audio_output = nil;
    AVAssetReaderTrackOutput *video_output = nil;
    const avb_video_decoder_plugin *custom_video_decoder = nullptr;
    void *custom_video_ctx = nullptr;

    int audio_stream_idx = -1;
    int video_stream_idx = -1;

    int sample_rate  = 0;   // effective output rate
    int channels     = 0;   // effective output channel count
    int audio_track_count = 0; // selectable audio tracks in the container
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
    double audio_buf_start_pts = -1.0;
    bool end_of_stream = false;

    std::vector<unsigned char> video_frame_buf;
};

AvbDecoderAVFoundation::AvbDecoderAVFoundation() {
    m_impl = new Impl();
}

AvbDecoderAVFoundation::~AvbDecoderAVFoundation() {
    if (m_impl) {
        if (m_impl->reader) [m_impl->reader cancelReading];
        if (m_impl->custom_video_decoder) {
            if (m_impl->custom_video_decoder->close && m_impl->custom_video_ctx)
                m_impl->custom_video_decoder->close(m_impl->custom_video_ctx);
            m_impl->custom_video_decoder = nullptr;
            m_impl->custom_video_ctx = nullptr;
        }
        delete m_impl;
    }
}

const char *AvbDecoderAVFoundation::get_backend_name() const { return "avfoundation"; }
const char *AvbDecoderAVFoundation::get_last_error() const {
    return m_last_error.empty() ? nullptr : m_last_error.c_str();
}

avb_result AvbDecoderAVFoundation::open_custom_video_decoder(
    void *track_ptr,
    const avb_decode_options &options
) {
    AVAssetTrack *track = (__bridge AVAssetTrack *)track_ptr;
    FourCharCode codec = avb_track_codec(track);

    avb_video_stream_info stream{};
    stream.stream_index = m_impl->video_stream_idx;
    stream.width = m_impl->width;
    stream.height = m_impl->height;
    stream.frame_rate = m_impl->frame_rate;
    stream.duration_sec = m_impl->duration_sec;
    stream.codec_tag = avb_bswap32((uint32_t)codec);
    m_impl->video_codec_name = avb_fourcc_to_name(codec);
    stream.codec_name = m_impl->video_codec_name.empty()
        ? nullptr : m_impl->video_codec_name.c_str();
    stream.time_base_num = 1;
    stream.time_base_den = 600;

    const avb_video_decoder_plugin *plugin =
        avb_find_video_decoder_plugin(stream, options);
    if (!plugin) return AVB_ERROR_STREAM_NOT_FOUND;

    void *ctx = nullptr;
    avb_result res = plugin->open(&ctx, &stream, &options);
    if (res != AVB_OK) {
        m_last_error = "Custom video decoder failed to open.";
        return res;
    }

    m_impl->video_output = [AVAssetReaderTrackOutput
        assetReaderTrackOutputWithTrack:track
        outputSettings:nil];
    m_impl->video_output.alwaysCopiesSampleData = YES;
    [m_impl->reader addOutput:m_impl->video_output];
    m_impl->custom_video_decoder = plugin;
    m_impl->custom_video_ctx = ctx;
    return AVB_OK;
}

avb_result AvbDecoderAVFoundation::open_file(const char *path, const avb_decode_options &options) {
    @autoreleasepool {
        if (options.video_memory != AVB_VIDEO_MEMORY_CPU ||
            options.hardware_policy == AVB_HARDWARE_REQUIRE) {
            m_last_error = "AVFoundation native hardware video frames are not implemented yet.";
            return AVB_ERROR_OPEN_FAILED;
        }
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
            m_impl->audio_track_count = (int)audio_tracks.count;
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
                m_impl->video_stream_idx = idx;

                CGSize size = track.naturalSize;
                m_impl->width  = (int)size.width;
                m_impl->height = (int)size.height;
                m_impl->frame_rate   = track.nominalFrameRate;
                m_impl->video_codec_name = avb_fourcc_to_name(avb_track_codec(track));

                if (options.enable_custom_video_decoders) {
                    avb_result custom_res =
                        open_custom_video_decoder((__bridge void *)track, options);
                    if (custom_res != AVB_OK &&
                        custom_res != AVB_ERROR_STREAM_NOT_FOUND) {
                        return custom_res;
                    }
                }

                if (!m_impl->custom_video_decoder) {
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
                }
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

        m_impl->end_of_stream = false;
        return AVB_OK;
    }
}

avb_result AvbDecoderAVFoundation::get_media_info(avb_media_info &out_info) {
    if (!m_impl->reader) return AVB_ERROR_INVALID_ARGUMENT;

    out_info = {};
    out_info.backend_name  = "avfoundation";
    out_info.duration_sec  = m_impl->duration_sec;

    if (m_impl->audio_output) {
        out_info.audio.available    = 1;
        out_info.audio.stream_index = m_impl->audio_stream_idx;
        out_info.audio.track_count  = m_impl->audio_track_count;
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

avb_result AvbDecoderAVFoundation::seek(double seconds) {
    if (!m_impl->reader) return AVB_ERROR_INVALID_ARGUMENT;

    // AVAssetReader does not support seeking directly — recreate it with a time range.
    @autoreleasepool {
        [m_impl->reader cancelReading];
        m_impl->audio_buf.clear();
        m_impl->audio_buf_pos = 0;
        m_impl->audio_buf_start_pts = -1.0;

        if (m_impl->duration_sec > 0.0 && seconds >= m_impl->duration_sec) {
            m_impl->end_of_stream = true;
            if (m_impl->custom_video_decoder && m_impl->custom_video_decoder->flush)
                m_impl->custom_video_decoder->flush(m_impl->custom_video_ctx);
            return AVB_OK;
        }
        m_impl->end_of_stream = false;

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
            NSDictionary *settings = m_impl->custom_video_decoder
                ? nil
                : @{ (NSString *)kCVPixelBufferPixelFormatTypeKey:
                         @(m_impl->cv_pixel_format) };
            m_impl->video_output = [AVAssetReaderTrackOutput
                assetReaderTrackOutputWithTrack:track
                outputSettings:settings];
            m_impl->video_output.alwaysCopiesSampleData =
                m_impl->custom_video_decoder ? YES : NO;
            [m_impl->reader addOutput:m_impl->video_output];
        }

        if (![m_impl->reader startReading]) {
            m_last_error = "AVAssetReader startReading failed after seek.";
            return AVB_ERROR_SEEK_FAILED;
        }

        if (m_impl->custom_video_decoder && m_impl->custom_video_decoder->flush)
            m_impl->custom_video_decoder->flush(m_impl->custom_video_ctx);
    }

    return AVB_OK;
}

bool AvbDecoderAVFoundation::fill_audio_buffer() {
    if (!m_impl->audio_output || !m_impl->audio_buf.empty()) return false;

    @autoreleasepool {
        CMSampleBufferRef sample_buf =
            [m_impl->audio_output copyNextSampleBuffer];
        if (!sample_buf) return false;

        CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample_buf);
        CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample_buf);
        size_t len = CMBlockBufferGetDataLength(block);
        size_t n_floats = len / sizeof(float);
        m_impl->audio_buf.resize(n_floats);
        m_impl->audio_buf_pos = 0;
        m_impl->audio_buf_start_pts = CMTIME_IS_VALID(pts) ? CMTimeGetSeconds(pts) : -1.0;
        CMBlockBufferCopyDataBytes(block, 0, len, m_impl->audio_buf.data());

        CFRelease(sample_buf);
        return true;
    }
}

double AvbDecoderAVFoundation::audio_next_pts() {
    if (m_impl->end_of_stream) return -1.0;
    if (!m_impl->audio_output || m_impl->channels <= 0 || m_impl->sample_rate <= 0)
        return -1.0;
    if (m_impl->audio_buf.empty()) fill_audio_buffer();
    if (m_impl->audio_buf.empty() || m_impl->audio_buf_start_pts < 0.0)
        return -1.0;

    double frames_offset = (double)m_impl->audio_buf_pos / (double)m_impl->channels;
    return m_impl->audio_buf_start_pts + frames_offset / (double)m_impl->sample_rate;
}

int AvbDecoderAVFoundation::read_audio_f32(float *dst_interleaved, int frames) {
    if (m_impl->end_of_stream) return 0;
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
                m_impl->audio_buf_start_pts = -1.0;
            }
            continue;
        }

        if (!fill_audio_buffer()) break;
    }

    return samples_written / nb_channels;
}

avb_result AvbDecoderAVFoundation::read_custom_video_frame(avb_video_frame &out_frame) {
    if (!m_impl->custom_video_decoder || !m_impl->custom_video_ctx)
        return AVB_ERROR_STREAM_NOT_FOUND;

    while (true) {
        @autoreleasepool {
            CMSampleBufferRef sample_buf =
                [m_impl->video_output copyNextSampleBuffer];
            if (!sample_buf) return AVB_ERROR_EOF;

            CMSampleBufferMakeDataReady(sample_buf);
            CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample_buf);
            if (!block) {
                CFRelease(sample_buf);
                continue;
            }

            size_t len = CMBlockBufferGetDataLength(block);
            if (len == 0) {
                CFRelease(sample_buf);
                continue;
            }
            m_impl->video_frame_buf.resize(len);
            CMBlockBufferCopyDataBytes(block, 0, len, m_impl->video_frame_buf.data());

            CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample_buf);
            CMTime dts = CMSampleBufferGetDecodeTimeStamp(sample_buf);
            CMTime dur = CMSampleBufferGetDuration(sample_buf);

            avb_encoded_packet packet{};
            packet.data = m_impl->video_frame_buf.data();
            packet.size = (int)len;
            packet.pts_sec = CMTIME_IS_VALID(pts) ? CMTimeGetSeconds(pts) : -1.0;
            packet.duration_sec = CMTIME_IS_VALID(dur) ? CMTimeGetSeconds(dur) : -1.0;
            packet.stream_index = m_impl->video_stream_idx;
            packet.pts = CMTIME_IS_VALID(pts) ? pts.value : -1;
            packet.dts = CMTIME_IS_VALID(dts) ? dts.value : packet.pts;
            packet.duration = CMTIME_IS_VALID(dur) ? dur.value : -1;
            packet.time_base_num = 1;
            packet.time_base_den = CMTIME_IS_VALID(pts) ? (int)pts.timescale : 1;
            packet.keyframe = 1;

            CFArrayRef attachments =
                CMSampleBufferGetSampleAttachmentsArray(sample_buf, false);
            if (attachments && CFArrayGetCount(attachments) > 0) {
                CFDictionaryRef dict =
                    (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
                packet.keyframe =
                    !CFDictionaryContainsKey(dict, kCMSampleAttachmentKey_NotSync);
            }

            avb_result res = m_impl->custom_video_decoder->decode_packet(
                m_impl->custom_video_ctx, &packet, &out_frame);
            CFRelease(sample_buf);
            if (res == AVB_OK) {
                if (out_frame.pts_sec < 0.0) out_frame.pts_sec = packet.pts_sec;
                return AVB_OK;
            }
            if (res == AVB_ERROR_AGAIN) continue;
            return res;
        }
    }
}

avb_result AvbDecoderAVFoundation::read_video_frame(avb_video_frame &out_frame) {
    if (m_impl->end_of_stream) return AVB_ERROR_EOF;
    if (m_impl->custom_video_decoder) return read_custom_video_frame(out_frame);
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
        out_frame.memory_type = AVB_VIDEO_MEMORY_CPU;
        out_frame.hardware_device = AVB_HW_DEVICE_AUTO;
        out_frame.plane_count = (int)plane_count;
        out_frame.data_size   = (int)total;
        for (int p = 0; p < AVB_MAX_PLANES; ++p) out_frame.dmabuf_fd[p] = -1;

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

void AvbDecoderAVFoundation::release_video_frame(avb_video_frame &frame) {
    if (m_impl->custom_video_decoder && m_impl->custom_video_decoder->release_frame) {
        m_impl->custom_video_decoder->release_frame(m_impl->custom_video_ctx, &frame);
        return;
    }
    memset(&frame, 0, sizeof(frame));
}

#else // !__APPLE__

AvbDecoderAVFoundation::AvbDecoderAVFoundation() {
    m_impl = new Impl();
    m_last_error = "AVFoundation backend is only available on Apple platforms.";
}
AvbDecoderAVFoundation::~AvbDecoderAVFoundation() { delete m_impl; }
const char *AvbDecoderAVFoundation::get_backend_name() const { return "avfoundation"; }
const char *AvbDecoderAVFoundation::get_last_error() const {
    return m_last_error.empty() ? nullptr : m_last_error.c_str();
}
avb_result AvbDecoderAVFoundation::open_file(const char *, const avb_decode_options &) {
    return AVB_ERROR_BACKEND_NOT_AVAILABLE;
}
avb_result AvbDecoderAVFoundation::get_media_info(avb_media_info &) {
    return AVB_ERROR_BACKEND_NOT_AVAILABLE;
}
avb_result AvbDecoderAVFoundation::seek(double) { return AVB_ERROR_BACKEND_NOT_AVAILABLE; }
int AvbDecoderAVFoundation::read_audio_f32(float *, int) { return 0; }
double AvbDecoderAVFoundation::audio_next_pts() { return -1.0; }
avb_result AvbDecoderAVFoundation::read_video_frame(avb_video_frame &) {
    return AVB_ERROR_BACKEND_NOT_AVAILABLE;
}
void AvbDecoderAVFoundation::release_video_frame(avb_video_frame &) {}

#endif // __APPLE__
