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

typedef struct avb_video_frame {
    int width;
    int height;
    avb_pixel_format format;
    int stride;
    double pts_sec;
    unsigned char *data;
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

#ifdef __cplusplus
}
#endif
