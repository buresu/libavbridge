#pragma once

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <stdbool.h>

// Pointers to dynamically loaded FFmpeg functions
struct AvbFFmpegFuncs {
    // avformat
    int (*avformat_open_input)(AVFormatContext **, const char *, const AVInputFormat *, AVDictionary **);
    int (*avformat_find_stream_info)(AVFormatContext *, AVDictionary **);
    int (*av_find_best_stream)(AVFormatContext *, enum AVMediaType, int, int, const AVCodec **, int);
    int (*av_read_frame)(AVFormatContext *, AVPacket *);
    int (*av_seek_frame)(AVFormatContext *, int, int64_t, int);
    void (*avformat_close_input)(AVFormatContext **);
    void (*avformat_free_context)(AVFormatContext *);

    // avcodec
    const AVCodec *(*avcodec_find_decoder)(enum AVCodecID);
    AVCodecContext *(*avcodec_alloc_context3)(const AVCodec *);
    int (*avcodec_parameters_to_context)(AVCodecContext *, const AVCodecParameters *);
    int (*avcodec_open2)(AVCodecContext *, const AVCodec *, AVDictionary **);
    int (*avcodec_send_packet)(AVCodecContext *, const AVPacket *);
    int (*avcodec_receive_frame)(AVCodecContext *, AVFrame *);
    void (*avcodec_flush_buffers)(AVCodecContext *);
    void (*avcodec_free_context)(AVCodecContext **);
    AVPacket *(*av_packet_alloc)();
    void (*av_packet_free)(AVPacket **);
    void (*av_packet_unref)(AVPacket *);

    // avutil
    AVFrame *(*av_frame_alloc)();
    void (*av_frame_free)(AVFrame **);
    void (*av_frame_unref)(AVFrame *);
    int (*av_frame_get_buffer)(AVFrame *, int);
    int (*av_samples_get_buffer_size)(int *, int, int, enum AVSampleFormat, int);
    void *(*av_malloc)(size_t);
    void (*av_free)(void *);
    char *(*av_strdup)(const char *);
    int (*av_strerror)(int, char *, size_t);
    int (*av_get_channel_layout_nb_channels)(uint64_t);
    void (*av_channel_layout_uninit)(AVChannelLayout *);
    int (*av_channel_layout_copy)(AVChannelLayout *, const AVChannelLayout *);

    // swresample
    SwrContext *(*swr_alloc)();
    int (*swr_alloc_set_opts2)(SwrContext **, const AVChannelLayout *, enum AVSampleFormat, int,
                                const AVChannelLayout *, enum AVSampleFormat, int, int, void *);
    int (*swr_init)(SwrContext *);
    int (*swr_convert)(SwrContext *, uint8_t **, int, const uint8_t **, int);
    void (*swr_free)(SwrContext **);
    int64_t (*swr_get_delay)(SwrContext *, int64_t);

    // swscale
    SwsContext *(*sws_getContext)(int, int, enum AVPixelFormat, int, int, enum AVPixelFormat,
                                  int, SwsFilter *, SwsFilter *, const double *);
    int (*sws_scale)(SwsContext *, const uint8_t *const [], const int [],
                     int, int, uint8_t *const [], const int []);
    void (*sws_freeContext)(SwsContext *);
};

bool avb_ffmpeg_load(AvbFFmpegFuncs &out_funcs, char *err_buf, int err_buf_size);
void avb_ffmpeg_unload();
