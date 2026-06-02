#include "avb_ffmpeg_loader.hpp"

#include <dlfcn.h>
#include <cstdio>
#include <cstring>

static void *g_handle_avformat   = nullptr;
static void *g_handle_avcodec    = nullptr;
static void *g_handle_avutil     = nullptr;
static void *g_handle_swresample = nullptr;
static void *g_handle_swscale    = nullptr;

static void *try_open(const char **names, int count) {
    for (int i = 0; i < count; i++) {
        void *h = dlopen(names[i], RTLD_LAZY | RTLD_GLOBAL);
        if (h) return h;
    }
    return nullptr;
}

#define LOAD_SYM(handle, funcs, name) \
    do { \
        (funcs).name = (decltype((funcs).name))dlsym(handle, #name); \
        if (!(funcs).name) { \
            snprintf(err_buf, err_buf_size, "Failed to load symbol: %s", #name); \
            return false; \
        } \
    } while(0)

bool avb_ffmpeg_load(AvbFFmpegFuncs &out_funcs, char *err_buf, int err_buf_size) {
    const char *avformat_names[] = {
        "libavformat.so.61", "libavformat.so.60", "libavformat.so.59", "libavformat.so"
    };
    const char *avcodec_names[] = {
        "libavcodec.so.61", "libavcodec.so.60", "libavcodec.so.59", "libavcodec.so"
    };
    const char *avutil_names[] = {
        "libavutil.so.59", "libavutil.so.58", "libavutil.so.57", "libavutil.so"
    };
    const char *swresample_names[] = {
        "libswresample.so.5", "libswresample.so.4", "libswresample.so.3", "libswresample.so"
    };
    const char *swscale_names[] = {
        "libswscale.so.8", "libswscale.so.7", "libswscale.so.6", "libswscale.so"
    };

    g_handle_avformat   = try_open(avformat_names,   4);
    g_handle_avcodec    = try_open(avcodec_names,    4);
    g_handle_avutil     = try_open(avutil_names,     4);
    g_handle_swresample = try_open(swresample_names, 4);
    g_handle_swscale    = try_open(swscale_names,    4);

    if (!g_handle_avformat || !g_handle_avcodec || !g_handle_avutil ||
        !g_handle_swresample || !g_handle_swscale) {
        snprintf(err_buf, err_buf_size,
            "FFmpeg backend unavailable: libavformat/libavcodec/libavutil/libswresample/libswscale "
            "could not be loaded. Install FFmpeg runtime libraries and try again.");
        avb_ffmpeg_unload();
        return false;
    }

    LOAD_SYM(g_handle_avformat, out_funcs, avformat_open_input);
    LOAD_SYM(g_handle_avformat, out_funcs, avformat_find_stream_info);
    LOAD_SYM(g_handle_avformat, out_funcs, av_find_best_stream);
    LOAD_SYM(g_handle_avformat, out_funcs, av_read_frame);
    LOAD_SYM(g_handle_avformat, out_funcs, av_seek_frame);
    LOAD_SYM(g_handle_avformat, out_funcs, avformat_close_input);
    LOAD_SYM(g_handle_avformat, out_funcs, avformat_free_context);

    LOAD_SYM(g_handle_avcodec, out_funcs, avcodec_find_decoder);
    LOAD_SYM(g_handle_avcodec, out_funcs, avcodec_alloc_context3);
    LOAD_SYM(g_handle_avcodec, out_funcs, avcodec_parameters_to_context);
    LOAD_SYM(g_handle_avcodec, out_funcs, avcodec_open2);
    LOAD_SYM(g_handle_avcodec, out_funcs, avcodec_send_packet);
    LOAD_SYM(g_handle_avcodec, out_funcs, avcodec_receive_frame);
    LOAD_SYM(g_handle_avcodec, out_funcs, avcodec_flush_buffers);
    LOAD_SYM(g_handle_avcodec, out_funcs, avcodec_free_context);
    LOAD_SYM(g_handle_avcodec, out_funcs, av_packet_alloc);
    LOAD_SYM(g_handle_avcodec, out_funcs, av_packet_free);
    LOAD_SYM(g_handle_avcodec, out_funcs, av_packet_unref);

    LOAD_SYM(g_handle_avutil, out_funcs, av_frame_alloc);
    LOAD_SYM(g_handle_avutil, out_funcs, av_frame_free);
    LOAD_SYM(g_handle_avutil, out_funcs, av_frame_unref);
    LOAD_SYM(g_handle_avutil, out_funcs, av_frame_get_buffer);
    LOAD_SYM(g_handle_avutil, out_funcs, av_samples_get_buffer_size);
    LOAD_SYM(g_handle_avutil, out_funcs, av_malloc);
    LOAD_SYM(g_handle_avutil, out_funcs, av_free);
    LOAD_SYM(g_handle_avutil, out_funcs, av_strdup);
    LOAD_SYM(g_handle_avutil, out_funcs, av_strerror);
    LOAD_SYM(g_handle_avutil, out_funcs, av_channel_layout_uninit);
    LOAD_SYM(g_handle_avutil, out_funcs, av_channel_layout_copy);
    LOAD_SYM(g_handle_avutil, out_funcs, av_channel_layout_default);

    LOAD_SYM(g_handle_swresample, out_funcs, swr_alloc);
    LOAD_SYM(g_handle_swresample, out_funcs, swr_alloc_set_opts2);
    LOAD_SYM(g_handle_swresample, out_funcs, swr_init);
    LOAD_SYM(g_handle_swresample, out_funcs, swr_convert);
    LOAD_SYM(g_handle_swresample, out_funcs, swr_free);
    LOAD_SYM(g_handle_swresample, out_funcs, swr_get_delay);

    LOAD_SYM(g_handle_swscale, out_funcs, sws_getContext);
    LOAD_SYM(g_handle_swscale, out_funcs, sws_scale);
    LOAD_SYM(g_handle_swscale, out_funcs, sws_freeContext);

    return true;
}

void avb_ffmpeg_unload() {
    if (g_handle_swscale)    { dlclose(g_handle_swscale);    g_handle_swscale    = nullptr; }
    if (g_handle_swresample) { dlclose(g_handle_swresample); g_handle_swresample = nullptr; }
    if (g_handle_avcodec)    { dlclose(g_handle_avcodec);    g_handle_avcodec    = nullptr; }
    if (g_handle_avformat)   { dlclose(g_handle_avformat);   g_handle_avformat   = nullptr; }
    if (g_handle_avutil)     { dlclose(g_handle_avutil);     g_handle_avutil     = nullptr; }
}
