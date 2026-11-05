#include "avb_decoder_avfoundation.hh"
#include "avb_plane_layout.hpp"
#include "avb_video_plugins.hpp"

#ifdef __APPLE__

#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <vector>

// Run an AVFoundation call sequence, turning an Objective-C exception into an
// error result instead of letting it unwind through the C API.
template <class Fn>
static avb_result avb_objc_guard(std::string &error, avb_result failure, Fn &&fn) {
    @try {
        return fn();
    } @catch (NSException *e) {
        const char *reason = e.reason.UTF8String;
        error = std::string("AVFoundation raised an exception: ") +
                (reason ? reason : e.name.UTF8String);
        return failure;
    }
}

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

// Reverse mapping, used to report the format of an external CVPixelBuffer handed
// back to the caller for AVB_VIDEO_MEMORY_EXTERNAL output.
static avb_pixel_format avb_avbfmt_from_cv(OSType cvfmt) {
    switch (cvfmt) {
        case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
        case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
            return AVB_PIXEL_FORMAT_NV12;
        case kCVPixelFormatType_420YpCbCr8Planar:
        case kCVPixelFormatType_420YpCbCr8PlanarFullRange:
            return AVB_PIXEL_FORMAT_I420;
        case kCVPixelFormatType_32BGRA:
            return AVB_PIXEL_FORMAT_BGRA8;
        default:
            return AVB_PIXEL_FORMAT_UNKNOWN;
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

// Build the AVAssetReaderTrackOutput settings for a CVPixelBuffer of the given
// format. External output requests IOSurface backing so the decoder's buffer
// can be handed back zero-copy.
static NSDictionary *avb_video_settings(OSType cv_pixel_format, bool external) {
    NSMutableDictionary *s = [@{
        (NSString *)kCVPixelBufferPixelFormatTypeKey: @(cv_pixel_format),
    } mutableCopy];
    if (external)
        s[(NSString *)kCVPixelBufferIOSurfacePropertiesKey] = @{};
    return s;
}

// Source-codec FourCC of a track's first format description, or 0 if unavailable.
static FourCharCode avb_track_codec(AVAssetTrack *track) {
    NSArray *formats = track.formatDescriptions;
    if (formats.count == 0) return 0;
    CMFormatDescriptionRef fmt = (__bridge CMFormatDescriptionRef)formats[0];
    return CMFormatDescriptionGetMediaSubType(fmt);
}

// The codec configuration record a track's sample description carries (avcC,
// hvcC, ...): what FFmpeg calls extradata. Empty when there is none.
static std::vector<unsigned char> avb_track_extradata(AVAssetTrack *track) {
    std::vector<unsigned char> out;
    NSArray *formats = track.formatDescriptions;
    if (formats.count == 0) return out;
    CMFormatDescriptionRef fmt = (__bridge CMFormatDescriptionRef)formats[0];
    NSDictionary *atoms = (__bridge NSDictionary *)CMFormatDescriptionGetExtension(
        fmt, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
    if (![atoms isKindOfClass:[NSDictionary class]]) return out;

    id record = nil;
    for (NSString *name in @[ @"avcC", @"hvcC", @"av1C", @"vpcC", @"glbl" ]) {
        record = atoms[name];
        if (record) break;
    }
    // A codec this backend does not name: the record is the only atom there.
    if (!record && atoms.count == 1) record = atoms.allValues.firstObject;
    if ([record isKindOfClass:[NSArray class]]) record = [record firstObject];
    if (![record isKindOfClass:[NSData class]]) return out;
    NSData *data = record;
    const auto *bytes = static_cast<const unsigned char *>(data.bytes);
    out.assign(bytes, bytes + data.length);
    return out;
}

// Whether VideoToolbox decodes this track on hardware. AVAssetReader builds the
// decompression session itself and exposes neither the session nor a switch
// for it (decoder specification keys handed in through
// AVVideoDecompressionPropertiesKey are ignored), so this builds one the same
// way -- no specification, the choice VideoToolbox's -- and asks what it chose.
static bool avb_decodes_on_hardware(AVAssetTrack *track) {
    NSArray *formats = track.formatDescriptions;
    if (formats.count == 0) return false;
    CMVideoFormatDescriptionRef format =
        (__bridge CMVideoFormatDescriptionRef)formats[0];
    if (@available(macOS 10.9, iOS 17.0, tvOS 17.0, *)) {
        VTDecompressionSessionRef session = NULL;
        if (VTDecompressionSessionCreate(kCFAllocatorDefault, format, NULL,
                                         NULL, NULL, &session) != noErr ||
            !session)
            return false;
        // A software decoder does not know the property at all.
        CFBooleanRef value = NULL;
        bool hardware =
            VTSessionCopyProperty(
                session,
                kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,
                kCFAllocatorDefault, &value) == noErr &&
            value && CFBooleanGetValue(value);
        if (value) CFRelease(value);
        VTDecompressionSessionInvalidate(session);
        CFRelease(session);
        return hardware;
    }
    // Before iOS 17 a session cannot be asked; the codec can.
    return VTIsHardwareDecodeSupported(
        CMFormatDescriptionGetMediaSubType(format));
}

// The YUV encoding a decoded buffer declares: the range is the pixel format's,
// the matrix an attachment VideoToolbox carries over from the stream. RGB
// buffers are already converted and have nothing left to declare.
static void avb_fill_color(CVPixelBufferRef pb, avb_video_frame &frame) {
    switch (CVPixelBufferGetPixelFormatType(pb)) {
        case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
        case kCVPixelFormatType_420YpCbCr8Planar:
            frame.color_range = AVB_COLOR_RANGE_LIMITED;
            break;
        case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
        case kCVPixelFormatType_420YpCbCr8PlanarFullRange:
            frame.color_range = AVB_COLOR_RANGE_FULL;
            break;
        default:
            return;
    }
    CFTypeRef matrix =
        CVBufferCopyAttachment(pb, kCVImageBufferYCbCrMatrixKey, NULL);
    if (!matrix) return;
    if (CFEqual(matrix, kCVImageBufferYCbCrMatrix_ITU_R_601_4))
        frame.color_matrix = AVB_COLOR_MATRIX_BT601;
    else if (CFEqual(matrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2))
        frame.color_matrix = AVB_COLOR_MATRIX_BT709;
    else if (CFEqual(matrix, kCVImageBufferYCbCrMatrix_ITU_R_2020))
        frame.color_matrix = AVB_COLOR_MATRIX_BT2020_NCL;
    CFRelease(matrix);
}

static uint32_t avb_bswap32(uint32_t v) {
    return ((v & 0x000000ffu) << 24) |
           ((v & 0x0000ff00u) << 8)  |
           ((v & 0x00ff0000u) >> 8)  |
           ((v & 0xff000000u) >> 24);
}

// Interleaved float32 PCM at `rate` with `channels` channels.
static AudioStreamBasicDescription avb_float_asbd(double rate, int channels) {
    AudioStreamBasicDescription asbd{};
    asbd.mSampleRate       = rate;
    asbd.mFormatID         = kAudioFormatLinearPCM;
    asbd.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    asbd.mChannelsPerFrame = (UInt32)channels;
    asbd.mBitsPerChannel   = 32;
    asbd.mBytesPerFrame    = (UInt32)channels * sizeof(float);
    asbd.mFramesPerPacket  = 1;
    asbd.mBytesPerPacket   = asbd.mBytesPerFrame;
    return asbd;
}

struct AvbDecoderAVFoundation::Impl {
    AVAsset *asset = nil;
    AVAssetReader *reader = nil;
    AVAssetTrack *audio_track = nil;
    AVAssetTrack *video_track = nil;
    AVAssetReaderTrackOutput *audio_output = nil;
    AVAssetReaderTrackOutput *video_output = nil;
    const avb_video_decoder_plugin *custom_video_decoder = nullptr;
    void *custom_video_ctx = nullptr;
    std::vector<unsigned char> video_extradata;

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
    // CPU I420 is read as NV12 and split here: CoreVideo's conversion to
    // planar 4:2:0 leaves the last row of an odd-height picture unwritten.
    bool split_nv12_to_i420         = false;
    avb_video_memory_type video_memory = AVB_VIDEO_MEMORY_CPU;
    // What decodes the video: VIDEOTOOLBOX for its hardware decoder, AUTO for
    // software. Asking costs about as much as the rest of an open, so the
    // question runs beside it and whoever needs the answer waits for it.
    dispatch_group_t hw_probe = nil;
    avb_hardware_device video_hw_device = AVB_HW_DEVICE_AUTO;

    void probe_hardware(AVAssetTrack *track) {
        hw_probe = dispatch_group_create();
        dispatch_group_async(
            hw_probe, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                @autoreleasepool {
                    if (avb_decodes_on_hardware(track))
                        video_hw_device = AVB_HW_DEVICE_VIDEOTOOLBOX;
                }
            });
    }

    avb_hardware_device hardware_device() {
        if (hw_probe) {
            dispatch_group_wait(hw_probe, DISPATCH_TIME_FOREVER);
            hw_probe = nil;
        }
        return video_hw_device;
    }

    std::string audio_codec_name;
    std::string video_codec_name;

    std::vector<float> audio_buf;
    int audio_buf_pos = 0;
    double audio_buf_start_pts = -1.0;
    bool audio_failed = false;
    bool end_of_stream = false;

    // Rate conversion runs here rather than in AVAssetReader, whose resampler
    // drops the tail it still holds at the end of the stream. The reader
    // delivers the source rate; AudioConverter flushes everything it holds.
    AudioConverterRef resampler = nullptr;
    std::vector<float> resample_src;   // the reader block being consumed
    size_t resample_src_pos = 0;       // in frames
    double resample_start_pts = -1.0;  // source time of the first output frame
    int64_t resampled_frames = 0;      // output frames since start_pts
    bool resample_input_done = false;  // the reader has nothing more
    bool resample_drained = false;     // the converter has flushed its tail

    void reset_resampler() {
        if (resampler) AudioConverterReset(resampler);
        resample_src.clear();
        resample_src_pos = 0;
        resample_start_pts = -1.0;
        resampled_frames = 0;
        resample_input_done = false;
        resample_drained = false;
    }

    std::vector<unsigned char> video_frame_buf;

    ~Impl() {
        if (resampler) AudioConverterDispose(resampler);
    }
};

AvbDecoderAVFoundation::AvbDecoderAVFoundation() {
    m_impl = new Impl();
}

AvbDecoderAVFoundation::~AvbDecoderAVFoundation() {
    if (m_impl) {
        if (m_impl->reader) [m_impl->reader cancelReading];
        m_impl->hardware_device(); // the probe answers into the Impl
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

bool AvbDecoderAVFoundation::audio_read_failed() const {
    return m_impl && m_impl->audio_failed;
}

bool AvbDecoderAVFoundation::reader_failed() {
    AVAssetReader *reader = m_impl->reader;
    if (!reader || reader.status != AVAssetReaderStatusFailed) return false;
    NSError *error = reader.error;
    m_last_error = std::string("AVAssetReader failed: ") +
        (error ? error.localizedDescription.UTF8String : "unknown error");
    return true;
}

// Attach a track output to the reader, refusing one it would raise on.
static bool avb_add_output(AVAssetReader *reader, AVAssetReaderTrackOutput *output) {
    if (![reader canAddOutput:output]) return false;
    [reader addOutput:output];
    return true;
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
    m_impl->video_extradata = avb_track_extradata(track);
    if (!m_impl->video_extradata.empty()) {
        stream.extradata = m_impl->video_extradata.data();
        stream.extradata_size = (int)m_impl->video_extradata.size();
    }
    // Packets count in the track's media timescale.
    const CMTimeScale timescale = track.naturalTimeScale;
    stream.time_base_num = 1;
    stream.time_base_den = timescale > 0 ? timescale : 600;

    const avb_video_decoder_plugin *plugin =
        avb_find_video_decoder_plugin(stream, options);
    if (!plugin) return AVB_ERROR_STREAM_NOT_FOUND;

    AVAssetReaderTrackOutput *output = [AVAssetReaderTrackOutput
        assetReaderTrackOutputWithTrack:track
        outputSettings:nil];
    output.alwaysCopiesSampleData = NO;
    if (![m_impl->reader canAddOutput:output]) {
        m_last_error = "AVAssetReader cannot read the video track's samples.";
        return AVB_ERROR_OPEN_FAILED;
    }

    void *ctx = nullptr;
    avb_result res = plugin->open(&ctx, &stream, &options);
    if (res != AVB_OK) {
        m_last_error = "Custom video decoder failed to open.";
        return res;
    }

    [m_impl->reader addOutput:output];
    m_impl->video_output = output;
    m_impl->custom_video_decoder = plugin;
    m_impl->custom_video_ctx = ctx;
    return AVB_OK;
}

avb_result AvbDecoderAVFoundation::open_file(const char *path, const avb_decode_options &options) {
    return avb_objc_guard(m_last_error, AVB_ERROR_OPEN_FAILED,
                          [&] { return open_file_impl(path, options); });
}

avb_result AvbDecoderAVFoundation::open_file_impl(const char *path, const avb_decode_options &options) {
    @autoreleasepool {
        // CPU frames are copied out plane-by-plane; EXTERNAL hands back the
        // decoder's IOSurface-backed CVPixelBuffer (zero-copy, VideoToolbox).
        if (options.video_memory == AVB_VIDEO_MEMORY_EXTERNAL &&
            options.video_external_type !=
                AVB_VIDEO_EXTERNAL_CVPIXEL_BUFFER) {
            m_last_error =
                "AVFoundation external output requires CVPixelBuffer.";
            return AVB_ERROR_OPEN_FAILED;
        }
        m_impl->video_memory = options.video_memory;
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
            m_last_error = std::string("AVAssetReader creation failed") +
                (error ? std::string(": ") + error.localizedDescription.UTF8String
                       : std::string("."));
            return AVB_ERROR_OPEN_FAILED;
        }

        // Duration
        CMTime duration = m_impl->asset.duration;
        m_impl->duration_sec = CMTimeGetSeconds(duration);

        // Audio track. options.audio_stream_index selects the Nth audio track
        // (0-based); -1 picks the first. (Stream indices are necessarily
        // backend-specific; AVFoundation has no file-wide stream numbering.)
        // A track asked for by index must exist.
        if (options.enable_audio) {
            NSArray<AVAssetTrack *> *audio_tracks =
                avb_load_tracks(m_impl->asset, AVMediaTypeAudio);
            m_impl->audio_track_count = (int)audio_tracks.count;
            int idx = options.audio_stream_index >= 0 ? options.audio_stream_index : 0;
            if (options.audio_stream_index >= 0 && idx >= (int)audio_tracks.count) {
                m_last_error = "The requested audio track does not exist.";
                return AVB_ERROR_STREAM_NOT_FOUND;
            }
            if (idx < (int)audio_tracks.count) {
                AVAssetTrack *track = audio_tracks[idx];

                m_impl->req_sample_rate = options.audio_sample_rate;
                m_impl->req_channels    = options.audio_channels;

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

                // A rate change is ours to make when the source rate is known
                // (see Impl::resampler); the reader still remixes channels.
                int reader_rate = m_impl->req_sample_rate;
                if (m_impl->req_sample_rate > 0 && src_rate > 0 &&
                    m_impl->channels > 0) {
                    reader_rate = 0;
                    if (m_impl->req_sample_rate != src_rate) {
                        AudioStreamBasicDescription in =
                            avb_float_asbd(src_rate, m_impl->channels);
                        AudioStreamBasicDescription out =
                            avb_float_asbd(m_impl->req_sample_rate, m_impl->channels);
                        if (AudioConverterNew(&in, &out, &m_impl->resampler) != noErr) {
                            m_last_error = "AudioConverter creation for resampling failed.";
                            return AVB_ERROR_OPEN_FAILED;
                        }
                        UInt32 quality = kAudioConverterQuality_High;
                        AudioConverterSetProperty(
                            m_impl->resampler,
                            kAudioConverterSampleRateConverterQuality,
                            sizeof(quality), &quality);
                        // Zero latency: output starts with the first input
                        // sample, so its timestamps are the source's.
                        UInt32 prime = kConverterPrimeMethod_Normal;
                        AudioConverterSetProperty(
                            m_impl->resampler, kAudioConverterPrimeMethod,
                            sizeof(prime), &prime);
                    }
                }

                m_impl->audio_output = [AVAssetReaderTrackOutput
                    assetReaderTrackOutputWithTrack:track
                    outputSettings:avb_audio_settings(reader_rate,
                                                      m_impl->req_channels)];
                m_impl->audio_output.alwaysCopiesSampleData = NO;
                if (!avb_add_output(m_impl->reader, m_impl->audio_output)) {
                    m_last_error = "AVAssetReader cannot decode the audio track.";
                    return AVB_ERROR_OPEN_FAILED;
                }
                m_impl->audio_track = track;
                m_impl->audio_stream_idx = idx;
                m_impl->audio_codec_name = avb_fourcc_to_name(avb_track_codec(track));
            }
        }

        // Video track (see audio note above for stream-index semantics).
        if (options.enable_video) {
            NSArray<AVAssetTrack *> *video_tracks =
                avb_load_tracks(m_impl->asset, AVMediaTypeVideo);
            int idx = options.video_stream_index >= 0 ? options.video_stream_index : 0;
            if (options.video_stream_index >= 0 && idx >= (int)video_tracks.count) {
                m_last_error = "The requested video track does not exist.";
                return AVB_ERROR_STREAM_NOT_FOUND;
            }
            if (idx < (int)video_tracks.count) {
                AVAssetTrack *track = video_tracks[idx];
                m_impl->video_stream_idx = idx;
                m_impl->video_track = track;

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
                    // The decoder is VideoToolbox's choice, which the policy
                    // cannot change: REQUIRE checks it, and the frames say
                    // what it was under every policy.
                    m_impl->probe_hardware(track);
                    if (options.hardware_policy == AVB_HARDWARE_REQUIRE &&
                        m_impl->hardware_device() !=
                            AVB_HW_DEVICE_VIDEOTOOLBOX) {
                        m_last_error =
                            "VideoToolbox has no hardware decoder for this stream.";
                        return AVB_ERROR_OPEN_FAILED;
                    }

                    bool external = m_impl->video_memory == AVB_VIDEO_MEMORY_EXTERNAL;
                    // External output defaults to NV12, the format VideoToolbox
                    // decodes into, so the IOSurface is handed back untouched.
                    avb_pixel_format want = options.video_format;
                    if (want == AVB_PIXEL_FORMAT_UNKNOWN)
                        want = external ? AVB_PIXEL_FORMAT_NV12 : AVB_PIXEL_FORMAT_BGRA8;
                    m_impl->video_avb_fmt = want;
                    m_impl->split_nv12_to_i420 =
                        !external && want == AVB_PIXEL_FORMAT_I420;
                    m_impl->cv_pixel_format = avb_cvpixfmt(
                        m_impl->split_nv12_to_i420 ? AVB_PIXEL_FORMAT_NV12 : want);
                    // RGBA is emulated by swizzling a BGRA CPU copy, which is not
                    // possible for a zero-copy external buffer.
                    m_impl->swizzle_rgba = !external &&
                        (options.video_format == AVB_PIXEL_FORMAT_RGBA8);

                    m_impl->video_output = [AVAssetReaderTrackOutput
                        assetReaderTrackOutputWithTrack:track
                        outputSettings:avb_video_settings(m_impl->cv_pixel_format,
                                                          external)];
                    m_impl->video_output.alwaysCopiesSampleData = NO;
                    if (!avb_add_output(m_impl->reader, m_impl->video_output)) {
                        m_impl->video_output = nil;
                        m_last_error = "AVAssetReader cannot decode the video track.";
                        return AVB_ERROR_OPEN_FAILED;
                    }
                }
            }
        }

        if (!m_impl->audio_output && !m_impl->video_output) {
            m_last_error = "No supported audio or video stream found.";
            return AVB_ERROR_STREAM_NOT_FOUND;
        }

        if (![m_impl->reader startReading]) {
            if (!reader_failed())
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
    return avb_objc_guard(m_last_error, AVB_ERROR_SEEK_FAILED,
                          [&] { return seek_impl(seconds); });
}

avb_result AvbDecoderAVFoundation::seek_impl(double seconds) {
    if (!m_impl->reader) return AVB_ERROR_INVALID_ARGUMENT;

    // AVAssetReader does not support seeking directly — recreate it with a time range.
    @autoreleasepool {
        [m_impl->reader cancelReading];
        m_impl->audio_buf.clear();
        m_impl->audio_buf_pos = 0;
        m_impl->audio_buf_start_pts = -1.0;
        m_impl->reset_resampler();

        if (m_impl->duration_sec > 0.0 && seconds >= m_impl->duration_sec) {
            m_impl->end_of_stream = true;
            m_impl->audio_failed = false;
            m_last_error.clear();
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
            m_impl->audio_output = [AVAssetReaderTrackOutput
                assetReaderTrackOutputWithTrack:m_impl->audio_track
                outputSettings:avb_audio_settings(
                    m_impl->resampler ? 0 : m_impl->req_sample_rate,
                    m_impl->req_channels)];
            m_impl->audio_output.alwaysCopiesSampleData = NO;
            if (!avb_add_output(m_impl->reader, m_impl->audio_output)) {
                m_last_error = "AVAssetReader cannot decode the audio track after seek.";
                return AVB_ERROR_SEEK_FAILED;
            }
        }

        if (m_impl->video_output) {
            NSDictionary *settings = nil;
            if (!m_impl->custom_video_decoder) {
                bool external = m_impl->video_memory == AVB_VIDEO_MEMORY_EXTERNAL;
                settings = avb_video_settings(m_impl->cv_pixel_format, external);
            }
            m_impl->video_output = [AVAssetReaderTrackOutput
                assetReaderTrackOutputWithTrack:m_impl->video_track
                outputSettings:settings];
            m_impl->video_output.alwaysCopiesSampleData = NO;
            if (!avb_add_output(m_impl->reader, m_impl->video_output)) {
                m_last_error = "AVAssetReader cannot decode the video track after seek.";
                return AVB_ERROR_SEEK_FAILED;
            }
        }

        if (![m_impl->reader startReading]) {
            if (!reader_failed())
                m_last_error = "AVAssetReader startReading failed after seek.";
            return AVB_ERROR_SEEK_FAILED;
        }

        m_impl->audio_failed = false;
        m_last_error.clear();
        if (m_impl->custom_video_decoder && m_impl->custom_video_decoder->flush)
            m_impl->custom_video_decoder->flush(m_impl->custom_video_ctx);
    }

    return AVB_OK;
}

bool AvbDecoderAVFoundation::copy_reader_audio(std::vector<float> &out, double &pts) {
    while (true) {
        @autoreleasepool {
            CMSampleBufferRef sample_buf = nullptr;
            @try {
                sample_buf = [m_impl->audio_output copyNextSampleBuffer];
            } @catch (NSException *e) {
                m_last_error = std::string("AVFoundation raised an exception: ") +
                    (e.reason ? e.reason.UTF8String : e.name.UTF8String);
                m_impl->audio_failed = true;
                return false;
            }
            if (!sample_buf) {
                if (reader_failed()) m_impl->audio_failed = true;
                return false;
            }

            CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample_buf);
            size_t len = block ? CMBlockBufferGetDataLength(block) : 0;
            size_t n_floats = len / sizeof(float);
            if (n_floats == 0) {
                CFRelease(sample_buf);
                continue;
            }
            CMTime t = CMSampleBufferGetPresentationTimeStamp(sample_buf);
            pts = CMTIME_IS_VALID(t) ? CMTimeGetSeconds(t) : -1.0;
            out.resize(n_floats);
            CMBlockBufferCopyDataBytes(block, 0, n_floats * sizeof(float), out.data());
            CFRelease(sample_buf);
            return true;
        }
    }
}

namespace {

// AudioConverter input: hand over the rest of the current reader block, pulling
// the next one when it is used up. Zero frames tells the converter the stream
// ended, and it flushes the tail it holds.
struct AvbResampleInput {
    std::vector<float> *src;
    size_t *pos;
    double *start_pts;
    bool *done;
    int channels;
    std::function<bool(std::vector<float> &, double &)> refill;
};

OSStatus avb_resample_input(AudioConverterRef, UInt32 *io_frames,
                            AudioBufferList *io_data,
                            AudioStreamPacketDescription **, void *user) {
    auto *in = static_cast<AvbResampleInput *>(user);
    const size_t ch = (size_t)in->channels;
    if (*in->pos * ch >= in->src->size() && !*in->done) {
        double pts = -1.0;
        *in->pos = 0;
        if (in->refill(*in->src, pts)) {
            if (*in->start_pts < 0.0) *in->start_pts = pts;
        } else {
            in->src->clear();
            *in->done = true;
        }
    }
    const size_t available =
        *in->done ? 0 : in->src->size() / ch - *in->pos;
    const size_t frames = std::min<size_t>(*io_frames, available);
    io_data->mNumberBuffers = 1;
    io_data->mBuffers[0].mNumberChannels = (UInt32)ch;
    io_data->mBuffers[0].mDataByteSize = (UInt32)(frames * ch * sizeof(float));
    io_data->mBuffers[0].mData =
        frames ? in->src->data() + *in->pos * ch : nullptr;
    *in->pos += frames;
    *io_frames = (UInt32)frames;
    return noErr;
}

} // namespace

bool AvbDecoderAVFoundation::fill_resampled_audio() {
    if (m_impl->resample_drained) return false;
    const int ch = m_impl->channels;
    const UInt32 capacity = 4096;
    m_impl->audio_buf.resize((size_t)capacity * ch);

    AvbResampleInput input{
        &m_impl->resample_src, &m_impl->resample_src_pos,
        &m_impl->resample_start_pts, &m_impl->resample_input_done, ch,
        [this](std::vector<float> &out, double &pts) {
            return copy_reader_audio(out, pts);
        }};
    AudioBufferList list{};
    list.mNumberBuffers = 1;
    list.mBuffers[0].mNumberChannels = (UInt32)ch;
    list.mBuffers[0].mDataByteSize = capacity * ch * sizeof(float);
    list.mBuffers[0].mData = m_impl->audio_buf.data();
    UInt32 frames = capacity;
    OSStatus st = AudioConverterFillComplexBuffer(
        m_impl->resampler, avb_resample_input, &input, &frames, &list, nullptr);
    if (st != noErr) {
        m_last_error = "AudioConverter resampling failed.";
        m_impl->audio_failed = true;
        frames = 0;
    }
    if (frames == 0) {
        m_impl->audio_buf.clear();
        m_impl->resample_drained = true;
        return false;
    }

    m_impl->audio_buf.resize((size_t)frames * ch);
    m_impl->audio_buf_pos = 0;
    m_impl->audio_buf_start_pts = m_impl->resample_start_pts < 0.0 ? -1.0
        : m_impl->resample_start_pts +
              (double)m_impl->resampled_frames / m_impl->sample_rate;
    m_impl->resampled_frames += frames;
    return true;
}

bool AvbDecoderAVFoundation::fill_audio_buffer() {
    if (!m_impl->audio_output || !m_impl->audio_buf.empty() ||
        m_impl->audio_failed || m_impl->channels <= 0)
        return false;
    if (m_impl->resampler) return fill_resampled_audio();

    double pts = -1.0;
    if (!copy_reader_audio(m_impl->audio_buf, pts)) {
        m_impl->audio_buf.clear();
        return false;
    }
    m_impl->audio_buf_pos = 0;
    m_impl->audio_buf_start_pts = pts;
    return true;
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
    if (nb_channels <= 0) return 0;
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
            if (!sample_buf)
                return reader_failed() ? AVB_ERROR_DECODE_FAILED : AVB_ERROR_EOF;

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

            // DTS and duration count in the PTS timescale the packet names.
            CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample_buf);
            CMTime dts = CMSampleBufferGetDecodeTimeStamp(sample_buf);
            CMTime dur = CMSampleBufferGetDuration(sample_buf);
            const bool has_pts = CMTIME_IS_NUMERIC(pts);
            if (has_pts) {
                if (CMTIME_IS_NUMERIC(dts))
                    dts = CMTimeConvertScale(dts, pts.timescale,
                                             kCMTimeRoundingMethod_RoundHalfAwayFromZero);
                if (CMTIME_IS_NUMERIC(dur))
                    dur = CMTimeConvertScale(dur, pts.timescale,
                                             kCMTimeRoundingMethod_RoundHalfAwayFromZero);
            }

            avb_encoded_packet packet{};
            packet.data = m_impl->video_frame_buf.data();
            packet.size = (int)len;
            packet.pts_sec = has_pts ? CMTimeGetSeconds(pts) : -1.0;
            packet.duration_sec = CMTIME_IS_NUMERIC(dur) ? CMTimeGetSeconds(dur) : -1.0;
            packet.stream_index = m_impl->video_stream_idx;
            packet.pts = has_pts ? pts.value : -1;
            packet.dts = has_pts && CMTIME_IS_NUMERIC(dts) ? dts.value : packet.pts;
            packet.duration = has_pts && CMTIME_IS_NUMERIC(dur) ? dur.value : -1;
            packet.time_base_num = 1;
            packet.time_base_den = has_pts ? (int)pts.timescale : 1;
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
    return avb_objc_guard(m_last_error, AVB_ERROR_DECODE_FAILED,
                          [&] { return read_video_frame_impl(out_frame); });
}

avb_result AvbDecoderAVFoundation::read_video_frame_impl(avb_video_frame &out_frame) {
    if (m_impl->end_of_stream) return AVB_ERROR_EOF;
    if (m_impl->custom_video_decoder) return read_custom_video_frame(out_frame);
    if (!m_impl->video_output) return AVB_ERROR_STREAM_NOT_FOUND;

    @autoreleasepool {
        CMSampleBufferRef sample_buf =
            [m_impl->video_output copyNextSampleBuffer];
        if (!sample_buf)
            return reader_failed() ? AVB_ERROR_DECODE_FAILED : AVB_ERROR_EOF;

        CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample_buf);

        CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample_buf);
        if (!image) {
            CFRelease(sample_buf);
            m_last_error = "AVAssetReader returned a video sample without an image.";
            return AVB_ERROR_DECODE_FAILED;
        }

        // External output: retain the decoder's IOSurface-backed CVPixelBuffer and
        // hand it back without copying. The caller keeps native_owner intact and
        // calls release_video_frame(), which releases our retain.
        if (m_impl->video_memory == AVB_VIDEO_MEMORY_EXTERNAL) {
            CVPixelBufferRef pb = (CVPixelBufferRef)image;
            CVPixelBufferRetain(pb);
            out_frame = {};
            out_frame.width       = (int)CVPixelBufferGetWidth(pb);
            out_frame.height      = (int)CVPixelBufferGetHeight(pb);
            out_frame.format      =
                avb_avbfmt_from_cv(CVPixelBufferGetPixelFormatType(pb));
            out_frame.pts_sec     = CMTimeGetSeconds(pts);
            out_frame.memory_type = AVB_VIDEO_MEMORY_EXTERNAL;
            out_frame.external_type =
                AVB_VIDEO_EXTERNAL_CVPIXEL_BUFFER;
            // A CVPixelBuffer from the software decoder is still one; the
            // frame says which decoder made it, as CPU frames do.
            out_frame.hardware_device = m_impl->hardware_device();
            avb_fill_color(pb, out_frame);
            out_frame.plane_count = 0;
            out_frame.native_handle = pb;
            out_frame.native_owner  = this;
            for (int p = 0; p < AVB_MAX_PLANES; ++p) out_frame.dmabuf_fd[p] = -1;
            CFRelease(sample_buf);
            return AVB_OK;
        }

        CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);

        size_t width  = CVPixelBufferGetWidth(image);
        size_t height = CVPixelBufferGetHeight(image);

        out_frame = {};
        out_frame.width       = (int)width;
        out_frame.height      = (int)height;
        out_frame.format      = m_impl->video_avb_fmt;
        out_frame.pts_sec     = CMTimeGetSeconds(pts);
        out_frame.memory_type = AVB_VIDEO_MEMORY_CPU;
        // A frame copied out of the GPU's decoder still says what decoded it.
        out_frame.hardware_device = m_impl->hardware_device();
        avb_fill_color(image, out_frame);
        for (int p = 0; p < AVB_MAX_PLANES; ++p) out_frame.dmabuf_fd[p] = -1;

        if (m_impl->split_nv12_to_i420) {
            // NV12 from the reader, split into tightly packed I420 planes.
            const AvbPlaneLayout layout = avb_plane_layout(
                AVB_PIXEL_FORMAT_I420, (int)width, (int)height, 1);
            m_impl->video_frame_buf.resize(layout.total);
            unsigned char *dst = m_impl->video_frame_buf.data();

            const auto *src_y = static_cast<const unsigned char *>(
                CVPixelBufferGetBaseAddressOfPlane(image, 0));
            const size_t src_y_stride = CVPixelBufferGetBytesPerRowOfPlane(image, 0);
            const auto *src_uv = static_cast<const unsigned char *>(
                CVPixelBufferGetBaseAddressOfPlane(image, 1));
            const size_t src_uv_stride = CVPixelBufferGetBytesPerRowOfPlane(image, 1);

            unsigned char *y = dst + layout.offset[0];
            for (int r = 0; r < layout.rows[0]; ++r)
                memcpy(y + (size_t)r * layout.stride[0],
                       src_y + (size_t)r * src_y_stride, (size_t)layout.stride[0]);
            unsigned char *u = dst + layout.offset[1];
            unsigned char *v = dst + layout.offset[2];
            for (int r = 0; r < layout.rows[1]; ++r) {
                const unsigned char *row = src_uv + (size_t)r * src_uv_stride;
                unsigned char *ur = u + (size_t)r * layout.stride[1];
                unsigned char *vr = v + (size_t)r * layout.stride[2];
                for (int x = 0; x < layout.stride[1]; ++x) {
                    ur[x] = row[2 * x];
                    vr[x] = row[2 * x + 1];
                }
            }

            out_frame.plane_count = 3;
            out_frame.data_size   = (int)layout.total;
            for (int p = 0; p < 3; ++p) {
                out_frame.plane_data[p]   = dst + layout.offset[p];
                out_frame.plane_stride[p] = layout.stride[p];
                out_frame.plane_offset[p] = (int)layout.offset[p];
            }
            out_frame.data   = out_frame.plane_data[0];
            out_frame.stride = out_frame.plane_stride[0];

            CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
            CFRelease(sample_buf);
            return AVB_OK;
        }

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

        out_frame.plane_count = (int)plane_count;
        out_frame.data_size   = (int)total;

        size_t offset = 0;
        for (size_t p = 0; p < plane_count; ++p) {
            memcpy(dst + offset, src_base[p], plane_size[p]);
            out_frame.plane_data[p]   = dst + offset;
            out_frame.plane_stride[p] = (int)src_stride[p];
            out_frame.plane_offset[p] = (int)offset;
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
    if (frame.memory_type == AVB_VIDEO_MEMORY_EXTERNAL &&
        frame.external_type == AVB_VIDEO_EXTERNAL_CVPIXEL_BUFFER &&
        frame.native_owner == this && frame.native_handle) {
        CVPixelBufferRelease((CVPixelBufferRef)frame.native_handle);
    }
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
bool AvbDecoderAVFoundation::audio_read_failed() const { return false; }
avb_result AvbDecoderAVFoundation::read_video_frame(avb_video_frame &) {
    return AVB_ERROR_BACKEND_NOT_AVAILABLE;
}
void AvbDecoderAVFoundation::release_video_frame(avb_video_frame &) {}

#endif // __APPLE__
