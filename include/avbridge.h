#pragma once

/*
 * libavbridge — a small, portable C API for decoding and encoding media across
 * platform media backends (Media Foundation, AVFoundation, GStreamer, FFmpeg).
 *
 * Conventions
 * -----------
 * - Functions returning avb_result use AVB_OK (0) for success and a negative
 *   avb_result on failure; avb_result_string() maps a code to a short name.
 * - const char* fields and getters (codec_name, backend_name, *_get_last_error)
 *   point to storage owned by the decoder/encoder and remain valid until that
 *   object is closed. Copy them if you need them to outlive it.
 * - A decoder or encoder object is NOT thread-safe: do not call into the same
 *   object from multiple threads concurrently. Distinct objects are independent.
 * - avb_decode_options_default()/avb_encode_options_default() return structs
 *   pre-filled with sensible defaults; prefer them over zero-initialisation,
 *   which (for decoding) would enable neither audio nor video.
 */

#include <stddef.h> /* size_t */
#include <stdint.h> /* int64_t, uint32_t */

#ifdef __cplusplus
extern "C" {
#endif

#define AVB_VERSION_MAJOR 0
#define AVB_VERSION_MINOR 3
#define AVB_VERSION_PATCH 0

#ifdef _WIN32
#  ifdef AVB_BUILDING_SHARED
#    define AVB_API __declspec(dllexport)
#  elif defined(AVB_USING_SHARED)
#    define AVB_API __declspec(dllimport)
#  else
#    define AVB_API
#  endif
#else
#  define AVB_API __attribute__((visibility("default")))
#endif

typedef enum avb_result {
    AVB_OK = 0,
    AVB_ERROR_UNKNOWN = -1,
    AVB_ERROR_INVALID_ARGUMENT = -2,
    AVB_ERROR_BACKEND_NOT_AVAILABLE = -3,
    AVB_ERROR_OPEN_FAILED = -4,
    AVB_ERROR_STREAM_NOT_FOUND = -5,
    AVB_ERROR_DECODE_FAILED = -6,
    AVB_ERROR_SEEK_FAILED = -7,
    AVB_ERROR_EOF = -8,
    AVB_ERROR_ENCODE_FAILED = -9,
    AVB_ERROR_AGAIN = -10,
} avb_result;

typedef enum avb_backend {
    AVB_BACKEND_AUTO = 0,
    AVB_BACKEND_MEDIAFOUNDATION,
    AVB_BACKEND_AVFOUNDATION,
    AVB_BACKEND_FFMPEG,
    AVB_BACKEND_GSTREAMER,
} avb_backend;

typedef enum avb_pixel_format {
    AVB_PIXEL_FORMAT_UNKNOWN = 0,
    AVB_PIXEL_FORMAT_RGBA8,  /* packed, 1 plane */
    AVB_PIXEL_FORMAT_BGRA8,  /* packed, 1 plane */
    AVB_PIXEL_FORMAT_NV12,   /* planar, 2 planes: Y, interleaved CbCr */
    AVB_PIXEL_FORMAT_I420,   /* planar, 3 planes: Y, Cb, Cr (half size) */
    AVB_PIXEL_FORMAT_BC1_RGBA, /* compressed, 4x4 blocks, 8 bytes/block */
    AVB_PIXEL_FORMAT_BC3_RGBA, /* compressed, 4x4 blocks, 16 bytes/block */
    AVB_PIXEL_FORMAT_BC4_R,    /* compressed, 4x4 blocks, 8 bytes/block */
    AVB_PIXEL_FORMAT_BC5_RG,   /* compressed, 4x4 blocks, 16 bytes/block */
    AVB_PIXEL_FORMAT_BC7_RGBA, /* compressed, 4x4 blocks, 16 bytes/block */
} avb_pixel_format;

/* Codecs usable for encoding. AUTO selects the backend default (H.264 video,
 * AAC audio). Video codecs: H264, HEVC, VP9, HAP. Audio codecs: AAC, OPUS. Passing a
 * codec of the wrong media kind (e.g. AAC for video) is an invalid argument, as
 * is a codec a particular backend/container cannot produce. */
typedef enum avb_codec {
    AVB_CODEC_AUTO = 0,
    AVB_CODEC_H264 = 1,
    AVB_CODEC_AAC  = 2,
    AVB_CODEC_HEVC = 3,
    AVB_CODEC_VP9  = 4,
    AVB_CODEC_OPUS = 5,
    AVB_CODEC_HAP  = 6,
} avb_codec;

/* ------------------------------------------------------------------------- *
 * Shared media types
 * ------------------------------------------------------------------------- */

#define AVB_MAX_PLANES 3

/* A video frame. Used both as decoder output and encoder input. As decoder
 * output the decoder owns the backing memory until avb_decoder_release_video_frame.
 * As encoder input the caller fills width/height/format and plane_data/plane_stride
 * for plane_count planes (data/stride alias plane 0); pts_sec may carry a
 * timestamp (see avb_encoder_write_video).
 *
 * For compressed block formats (BC1/BC3/BC4/BC5/BC7), plane_count is 1,
 * plane_data[0]/data points to the compressed block payload, data_size is the
 * payload byte size, and stride is the byte count for one row of 4x4 blocks. */
typedef struct avb_video_frame {
    int width;
    int height;
    avb_pixel_format format;
    double pts_sec;

    /* Valid planes: 1 for packed (RGBA8/BGRA8), 2 for NV12 (Y, CbCr),
     * 3 for I420 (Y, Cb, Cr). */
    int plane_count;
    unsigned char *plane_data[AVB_MAX_PLANES];
    int plane_stride[AVB_MAX_PLANES];

    /* Aliases for plane 0 (packed-format convenience). data_size is the total
     * size in bytes of the backing buffer across all planes. */
    unsigned char *data;
    int stride;
    int data_size;
} avb_video_frame;

typedef struct avb_audio_info {
    int available;
    int stream_index;
    int track_count; /* number of selectable audio tracks in the container
                      * (>=1 when audio is available); pick one with
                      * avb_decode_options::audio_stream_index */
    int sample_rate;
    int channels;
    double duration_sec;
    const char *codec_name; /* source codec, e.g. "aac" (valid until close) */
} avb_audio_info;

typedef struct avb_video_info {
    int available;
    int stream_index;
    int width;
    int height;
    double frame_rate;
    double duration_sec;
    const char *codec_name; /* source codec, e.g. "h264" (valid until close) */
} avb_video_info;

typedef struct avb_media_info {
    const char *backend_name;
    double duration_sec;
    avb_audio_info audio;
    avb_video_info video;
} avb_media_info;

/* ------------------------------------------------------------------------- *
 * Library introspection
 * ------------------------------------------------------------------------- */

/* "MAJOR.MINOR.PATCH" of the library build. */
AVB_API const char *avb_version_string(void);

/* Short, stable name for a backend ("auto", "mediafoundation", "avfoundation",
 * "ffmpeg", "gstreamer"), or NULL for an out-of-range value. */
AVB_API const char *avb_backend_name(avb_backend backend);

/* Parse a backend name (case-sensitive, as returned by avb_backend_name) into
 * *out. Returns AVB_ERROR_INVALID_ARGUMENT for an unknown name. */
AVB_API avb_result avb_backend_from_name(const char *name, avb_backend *out);

/* 1 if `backend` is compiled into this build (so it can be selected), else 0.
 * This reflects build configuration, not whether the backend's runtime
 * libraries are installed — that is only known once you open a file. AUTO
 * reports whether this platform has any default backend. */
AVB_API int avb_backend_is_available(avb_backend backend);

/* Short name for a result code, e.g. "AVB_ERROR_EOF". Never NULL. */
AVB_API const char *avb_result_string(avb_result result);

/* ------------------------------------------------------------------------- *
 * Decoding
 * ------------------------------------------------------------------------- */

typedef struct avb_decoder avb_decoder;

typedef struct avb_decode_options {
    avb_backend backend;
    int audio_stream_index; /* -1 = auto/default */
    int video_stream_index; /* -1 = auto/default */
    int enable_audio;
    int enable_video;
    /* Desired decoded video pixel format. AVB_PIXEL_FORMAT_UNKNOWN (0) selects
     * the backend default (BGRA8). */
    avb_pixel_format video_format;
    /* Desired decoded audio output format. 0 = keep the source value.
     * avb_decoder_read_audio_f32 produces interleaved float at this
     * rate/channel count, and avb_audio_info reports the effective values. */
    int audio_sample_rate;
    int audio_channels;
    /* 1 = allow registered custom video decoders to handle matching video
     * streams before the backend's built-in video decoder is opened. */
    int enable_custom_video_decoders;
} avb_decode_options;

/* Sensible defaults: AUTO backend, both audio and video enabled, default
 * stream selection and formats. Prefer this over zero-initialisation. */
AVB_API avb_decode_options avb_decode_options_default(void);

/* Open `path` for decoding. options may be NULL (uses the defaults above).
 * On failure a non-NULL *out_dec is still returned so the caller can read
 * avb_decoder_get_last_error(); it must still be released with
 * avb_decoder_close(). */
AVB_API avb_result avb_decoder_open(
    avb_decoder **out_dec,
    const char *path,
    const avb_decode_options *options
);

/* Custom-I/O source for decoding from something other than a file path (e.g.
 * bytes embedded in an application package). All callbacks receive the `user`
 * pointer passed to avb_decoder_open_io. */
typedef struct avb_io_callbacks {
    /* Read up to `size` bytes into `buf`. Return the number of bytes read,
     * 0 at end of stream, or a negative value on error. Required. */
    int (*read)(void *user, unsigned char *buf, int size);
    /* Seek to `offset` relative to `whence` (SEEK_SET/SEEK_CUR/SEEK_END from
     * <stdio.h>). Return the new absolute offset, or a negative value on error.
     * May be NULL for a non-seekable source (seeking and duration are then
     * limited, and some backends fall back to buffering the whole stream). */
    long long (*seek)(void *user, long long offset, int whence);
    /* Total size in bytes, or a negative value if unknown. May be NULL. */
    long long (*size)(void *user);
} avb_io_callbacks;

/* Open a custom I/O source for decoding. `cb` and `user` (and any data they
 * reference) must remain valid until avb_decoder_close. Backends that cannot
 * consume callbacks natively buffer the stream into a temporary file first; the
 * FFmpeg backend reads through the callbacks directly. */
AVB_API avb_result avb_decoder_open_io(
    avb_decoder **out_dec,
    const avb_io_callbacks *cb,
    void *user,
    const avb_decode_options *options
);

/* Open an in-memory media buffer for decoding. `data` is NOT copied: it must
 * remain valid until avb_decoder_close. Convenience wrapper over
 * avb_decoder_open_io. */
AVB_API avb_result avb_decoder_open_memory(
    avb_decoder **out_dec,
    const void *data,
    size_t size,
    const avb_decode_options *options
);

typedef struct avb_video_stream_info {
    int stream_index;
    int width;
    int height;
    double frame_rate;
    double duration_sec;
    const char *codec_name;
    uint32_t codec_tag; /* Container fourcc when available, e.g. 'Hap1'. */
    const unsigned char *extradata;
    int extradata_size;
    int time_base_num;
    int time_base_den;
} avb_video_stream_info;

typedef struct avb_encoded_packet {
    const unsigned char *data;
    int size;
    double pts_sec;
    double duration_sec;
    int keyframe;
    int stream_index;
    int64_t pts;
    int64_t dts;
    int64_t duration;
    int time_base_num;
    int time_base_den;
} avb_encoded_packet;

typedef struct avb_video_decoder_plugin {
    size_t struct_size; /* Set to sizeof(avb_video_decoder_plugin). */
    const char *name;
    int (*can_decode)(const avb_video_stream_info *stream,
                      const avb_decode_options *options);
    avb_result (*open)(void **out_ctx,
                       const avb_video_stream_info *stream,
                       const avb_decode_options *options);
    avb_result (*decode_packet)(void *ctx,
                                const avb_encoded_packet *packet,
                                avb_video_frame *out_frame);
    void (*release_frame)(void *ctx, avb_video_frame *frame);
    void (*flush)(void *ctx);
    void (*close)(void *ctx);
} avb_video_decoder_plugin;

/* Register a process-wide custom video decoder. The plugin struct and any
 * callback targets it references must remain valid until unregistered. The
 * first registered plugin whose can_decode() returns non-zero handles a stream.
 * Custom video decoders are used by backends that can expose compressed video
 * packets while still supplying demuxing and regular audio decoding. Do not
 * unregister a plugin while any decoder using it may still be open. */
AVB_API avb_result avb_register_video_decoder(
    const avb_video_decoder_plugin *plugin
);

AVB_API avb_result avb_unregister_video_decoder(
    const avb_video_decoder_plugin *plugin
);

typedef struct avb_video_encode_info {
    int width;
    int height;
    double frame_rate;
    avb_pixel_format input_format;
    avb_codec codec;
    int bitrate;
} avb_video_encode_info;

typedef struct avb_encoded_video_stream {
    avb_codec codec;
    uint32_t codec_tag; /* Container fourcc when available, e.g. 'Hap1'. */
    const char *codec_name;
    const char *gst_caps; /* Optional encoded caps for GStreamer appsrc. */
    const unsigned char *extradata;
    int extradata_size;
    int time_base_num;
    int time_base_den;
} avb_encoded_video_stream;

typedef struct avb_video_encoder_plugin {
    size_t struct_size; /* Set to sizeof(avb_video_encoder_plugin). */
    const char *name;
    int (*can_encode)(const avb_video_encode_info *info);
    avb_result (*open)(void **out_ctx,
                       const avb_video_encode_info *info,
                       avb_encoded_video_stream *out_stream);
    avb_result (*encode_frame)(void *ctx,
                               const avb_video_frame *frame,
                               double pts_sec,
                               avb_encoded_packet *out_packet);
    avb_result (*flush)(void *ctx, avb_encoded_packet *out_packet);
    void (*release_packet)(void *ctx, avb_encoded_packet *packet);
    void (*close)(void *ctx);
} avb_video_encoder_plugin;

/* Register a process-wide custom video encoder. When a video encode request is
 * matched by can_encode(), capable backends use the plugin for video
 * compression and still mux audio through their native encoders. Do not
 * unregister a plugin while any encoder using it may still be open. */
AVB_API avb_result avb_register_video_encoder(
    const avb_video_encoder_plugin *plugin
);

AVB_API avb_result avb_unregister_video_encoder(
    const avb_video_encoder_plugin *plugin
);

AVB_API avb_result avb_decoder_get_media_info(
    avb_decoder *dec,
    avb_media_info *out_info
);

/* Seek both the audio and video streams to ~`seconds`. The decoders are
 * flushed; the next video frame returned starts at or just after the target
 * (frames between the preceding keyframe and the target are dropped), and audio
 * resumes at approximately the target. Exact frame-accurate seeking is not
 * guaranteed.
 *
 * If `out_landed_sec` is non-NULL it receives the requested time clamped to
 * [0, duration]; the *exact* landing time is reported by the pts of the next
 * decoded video frame (or out_first_pts of the next audio read). */
AVB_API avb_result avb_decoder_seek(
    avb_decoder *dec,
    double seconds,
    double *out_landed_sec
);

/* Reading both streams: when a decoder has audio AND video enabled, read the two
 * streams roughly interleaved (as a player does). Draining one stream to EOF
 * while never reading the other is NOT supported — a backend may bound its
 * internal decoded-frame buffering and deadlock. For bulk per-stream decoding
 * (e.g. dump all audio, or all video), open a decoder with only that stream
 * enabled. */

/* Read up to `frames` interleaved float frames. Returns the number of frames
 * written, or 0 at end of stream / when no audio track is present (use
 * avb_decoder_audio_at_eof / avb_media_info.audio.available to distinguish).
 *
 * If `out_first_pts` is non-NULL it receives the presentation time (seconds) of
 * the first sample in this block, or a negative value if unknown (some backends
 * do not report audio timestamps). */
AVB_API int avb_decoder_read_audio_f32(
    avb_decoder *dec,
    float *dst_interleaved,
    int frames,
    double *out_first_pts
);

/* 1 if an audio track exists and has been fully consumed (the last
 * read_audio_f32 returned 0 because of end of stream), else 0. Returns 0 when
 * there is no audio track at all (check avb_media_info.audio.available for
 * that). Reset by avb_decoder_seek. */
AVB_API int avb_decoder_audio_at_eof(
    avb_decoder *dec
);

/* Read the next video frame. Returns AVB_ERROR_EOF at end of stream. A returned
 * frame must be released with avb_decoder_release_video_frame. */
AVB_API avb_result avb_decoder_read_video_frame(
    avb_decoder *dec,
    avb_video_frame *out_frame
);

AVB_API void avb_decoder_release_video_frame(
    avb_decoder *dec,
    avb_video_frame *frame
);

AVB_API const char *avb_decoder_get_last_error(
    avb_decoder *dec
);

AVB_API void avb_decoder_close(
    avb_decoder *dec
);

/* ------------------------------------------------------------------------- *
 * Encoding
 * ------------------------------------------------------------------------- */

typedef struct avb_encoder avb_encoder;

typedef struct avb_video_encode_params {
    int enable;
    int width;
    int height;
    double frame_rate;             /* used to derive PTS when none is given */
    avb_codec codec;               /* AUTO -> H.264; or H264/HEVC/VP9 */
    int bitrate;                   /* bits/sec, 0 = backend default */
    avb_pixel_format input_format; /* format of frames passed to write_video */
} avb_video_encode_params;

typedef struct avb_audio_encode_params {
    int enable;
    int sample_rate;
    int channels;
    avb_codec codec;             /* AUTO -> AAC; or AAC/OPUS */
    int bitrate;                 /* bits/sec, 0 = backend default */
} avb_audio_encode_params;

typedef struct avb_encode_options {
    avb_backend backend;
    avb_video_encode_params video;
    avb_audio_encode_params audio;
} avb_encode_options;

/* Defaults: AUTO backend, both tracks disabled (enable + fill the ones you
 * want), codecs AUTO. */
AVB_API avb_encode_options avb_encode_options_default(void);

/* Open an encoder writing to `path`; the container is inferred from the file
 * extension (.mp4/.mov/.m4a, and .webm/.mkv where the backend supports it).
 * When both audio and video are enabled, feed the two tracks roughly
 * interleaved in increasing-PTS order (the standard muxer contract); writing
 * one track far ahead of the other may fail rather than buffer unboundedly.
 *
 * On failure a non-NULL *out_enc is still returned so the caller can read
 * avb_encoder_get_last_error(); it must still be released with
 * avb_encoder_close(). */
AVB_API avb_result avb_encoder_open(
    avb_encoder **out_enc,
    const char *path,
    const avb_encode_options *options
);

/* Encode one video frame. The presentation timestamp is resolved in this order:
 *   1. the `pts_sec` argument, if >= 0;
 *   2. otherwise frame->pts_sec, if >= 0 (so decoded frames pass through);
 *   3. otherwise it is derived from the configured frame_rate and frame count.
 * frame->format must match avb_video_encode_params::input_format. */
AVB_API avb_result avb_encoder_write_video(
    avb_encoder *enc,
    const avb_video_frame *frame,
    double pts_sec
);

/* Encode interleaved float audio at the configured sample_rate/channels.
 * Timestamps are derived from the running sample count. */
AVB_API avb_result avb_encoder_write_audio_f32(
    avb_encoder *enc,
    const float *src_interleaved,
    int frames
);

/* Flush encoders and finalize the container. Only avb_encoder_close is valid
 * afterwards. */
AVB_API avb_result avb_encoder_finish(
    avb_encoder *enc
);

AVB_API const char *avb_encoder_get_last_error(
    avb_encoder *enc
);

AVB_API void avb_encoder_close(
    avb_encoder *enc
);

#ifdef __cplusplus
}
#endif
