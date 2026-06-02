#pragma once

#ifdef __cplusplus
extern "C" {
#endif

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

typedef struct avb_context avb_context;

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
} avb_result;

typedef enum avb_backend {
    AVB_BACKEND_AUTO = 0,
    AVB_BACKEND_MEDIAFOUNDATION,
    AVB_BACKEND_AVFOUNDATION,
    AVB_BACKEND_FFMPEG,
} avb_backend;

typedef enum avb_pixel_format {
    AVB_PIXEL_FORMAT_UNKNOWN = 0,
    AVB_PIXEL_FORMAT_RGBA8,
    AVB_PIXEL_FORMAT_BGRA8,
    AVB_PIXEL_FORMAT_NV12,
} avb_pixel_format;

typedef struct avb_open_options {
    avb_backend backend;
    int audio_stream_index; /* -1 = auto/default */
    int video_stream_index; /* -1 = auto/default */
    int enable_audio;
    int enable_video;
    /* Desired decoded video pixel format. AVB_PIXEL_FORMAT_UNKNOWN (0) selects
     * the backend default (BGRA8). */
    avb_pixel_format video_format;
    /* Desired decoded audio output format. 0 = keep the source value.
     * avb_read_audio_f32 produces interleaved float at this rate/channel count,
     * and avb_audio_info reports the effective (output) values. */
    int audio_sample_rate;
    int audio_channels;
} avb_open_options;

typedef struct avb_audio_info {
    int available;
    int stream_index;
    int sample_rate;
    int channels;
    double duration_sec;
    const char *codec_name;
} avb_audio_info;

typedef struct avb_video_info {
    int available;
    int stream_index;
    int width;
    int height;
    double frame_rate;
    double duration_sec;
    const char *codec_name;
} avb_video_info;

typedef struct avb_media_info {
    const char *backend_name;
    double duration_sec;
    avb_audio_info audio;
    avb_video_info video;
} avb_media_info;

#define AVB_MAX_PLANES 3

typedef struct avb_video_frame {
    int width;
    int height;
    avb_pixel_format format;
    double pts_sec;

    /* Number of valid planes: 1 for packed formats (RGBA8/BGRA8), 2 for NV12
     * (plane 0 = Y, plane 1 = interleaved CbCr). */
    int plane_count;
    unsigned char *plane_data[AVB_MAX_PLANES];
    int plane_stride[AVB_MAX_PLANES];

    /* Convenience aliases for plane 0, for packed-format consumers:
     *   data   == plane_data[0]
     *   stride == plane_stride[0]
     * data_size is the total size in bytes of the backing buffer (all planes). */
    unsigned char *data;
    int stride;
    int data_size;
} avb_video_frame;

AVB_API avb_result avb_open_file(
    avb_context **out_ctx,
    const char *path,
    const avb_open_options *options
);

AVB_API avb_result avb_get_media_info(
    avb_context *ctx,
    avb_media_info *out_info
);

AVB_API avb_result avb_seek(
    avb_context *ctx,
    double seconds
);

AVB_API int avb_read_audio_f32(
    avb_context *ctx,
    float *dst_interleaved,
    int frames
);

AVB_API avb_result avb_read_video_frame(
    avb_context *ctx,
    avb_video_frame *out_frame
);

AVB_API void avb_release_video_frame(
    avb_context *ctx,
    avb_video_frame *frame
);

AVB_API const char *avb_get_last_error(
    avb_context *ctx
);

AVB_API void avb_close(
    avb_context *ctx
);

/* ------------------------------------------------------------------------- *
 * Encoding
 * ------------------------------------------------------------------------- */

typedef struct avb_encoder avb_encoder;

typedef enum avb_codec {
    AVB_CODEC_AUTO = 0, /* container default: H.264 for video, AAC for audio */
    AVB_CODEC_H264,
    AVB_CODEC_AAC,
} avb_codec;

typedef struct avb_video_encode_params {
    int enable;
    int width;
    int height;
    double frame_rate;            /* used to derive PTS when pts_sec < 0 */
    avb_codec codec;             /* AVB_CODEC_AUTO -> H.264 */
    int bitrate;                 /* bits/sec, 0 = backend default */
    avb_pixel_format input_format; /* format of frames passed to write_video */
} avb_video_encode_params;

typedef struct avb_audio_encode_params {
    int enable;
    int sample_rate;
    int channels;
    avb_codec codec;             /* AVB_CODEC_AUTO -> AAC */
    int bitrate;                 /* bits/sec, 0 = backend default */
} avb_audio_encode_params;

typedef struct avb_encode_options {
    avb_backend backend;
    avb_video_encode_params video;
    avb_audio_encode_params audio;
} avb_encode_options;

/* When both audio and video are enabled, write the two tracks roughly
 * interleaved in increasing-PTS order (the standard muxer contract). Writing one
 * track far ahead of the other will fail rather than buffer unboundedly. */
AVB_API avb_result avb_encoder_open(
    avb_encoder **out_enc,
    const char *path,
    const avb_encode_options *options
);

/* Encode one video frame. pts_sec < 0 derives the timestamp from frame_rate. */
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
