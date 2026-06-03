#include "avb_ffmpeg_loader.hpp"

#include <cstdio>
#include <cstring>

// Cross-platform dynamic loading: LoadLibrary/GetProcAddress on Windows,
// dlopen/dlsym elsewhere. FFmpeg is loaded at runtime on every platform and
// never linked at build time.
#ifdef _WIN32
#  include <windows.h>
static void *lib_open(const char *name) { return (void *)LoadLibraryA(name); }
static void *lib_sym(void *h, const char *s) { return (void *)GetProcAddress((HMODULE)h, s); }
static void  lib_close(void *h) { FreeLibrary((HMODULE)h); }
#else
#  include <dlfcn.h>
static void *lib_open(const char *name) { return dlopen(name, RTLD_LAZY | RTLD_GLOBAL); }
static void *lib_sym(void *h, const char *s) { return dlsym(h, s); }
static void  lib_close(void *h) { dlclose(h); }
#endif

static void *g_handle_avformat   = nullptr;
static void *g_handle_avcodec    = nullptr;
static void *g_handle_avutil     = nullptr;
static void *g_handle_swresample = nullptr;
static void *g_handle_swscale    = nullptr;

static void *try_open(const char **names, int count) {
    for (int i = 0; i < count; i++) {
        void *h = lib_open(names[i]);
        if (h) return h;
    }
    return nullptr;
}

#define LOAD_SYM(handle, funcs, name) \
    do { \
        (funcs).name = (decltype((funcs).name))lib_sym(handle, #name); \
        if (!(funcs).name) { \
            snprintf(err_buf, err_buf_size, "Failed to load symbol: %s", #name); \
            return false; \
        } \
    } while(0)

bool avb_ffmpeg_load(AvbFFmpegFuncs &out_funcs, char *err_buf, int err_buf_size) {
    // The major version compiled against (from the FFmpeg headers) is tried
    // first, so the loaded library's struct ABI matches what this translation
    // unit expects. Older majors follow as a best-effort fallback. Naming is
    // platform-specific: "libavformat.so.<major>" / "avformat-<major>.dll" /
    // "libavformat.<major>.dylib". Keep the leading entry in sync with the
    // FFmpeg headers used at build time (see *_VERSION_MAJOR).
#if defined(_WIN32)
    const char *avformat_names[] = {
        "avformat-" AV_STRINGIFY(LIBAVFORMAT_VERSION_MAJOR) ".dll",
        "avformat-62.dll", "avformat-61.dll", "avformat-60.dll", "avformat-59.dll"
    };
    const char *avcodec_names[] = {
        "avcodec-" AV_STRINGIFY(LIBAVCODEC_VERSION_MAJOR) ".dll",
        "avcodec-62.dll", "avcodec-61.dll", "avcodec-60.dll", "avcodec-59.dll"
    };
    const char *avutil_names[] = {
        "avutil-" AV_STRINGIFY(LIBAVUTIL_VERSION_MAJOR) ".dll",
        "avutil-60.dll", "avutil-59.dll", "avutil-58.dll", "avutil-57.dll"
    };
    const char *swresample_names[] = {
        "swresample-" AV_STRINGIFY(LIBSWRESAMPLE_VERSION_MAJOR) ".dll",
        "swresample-6.dll", "swresample-5.dll", "swresample-4.dll", "swresample-3.dll"
    };
    const char *swscale_names[] = {
        "swscale-" AV_STRINGIFY(LIBSWSCALE_VERSION_MAJOR) ".dll",
        "swscale-9.dll", "swscale-8.dll", "swscale-7.dll", "swscale-6.dll"
    };
#elif defined(__APPLE__)
    const char *avformat_names[] = {
        "libavformat." AV_STRINGIFY(LIBAVFORMAT_VERSION_MAJOR) ".dylib",
        "libavformat.62.dylib", "libavformat.61.dylib", "libavformat.60.dylib",
        "libavformat.59.dylib", "libavformat.dylib"
    };
    const char *avcodec_names[] = {
        "libavcodec." AV_STRINGIFY(LIBAVCODEC_VERSION_MAJOR) ".dylib",
        "libavcodec.62.dylib", "libavcodec.61.dylib", "libavcodec.60.dylib",
        "libavcodec.59.dylib", "libavcodec.dylib"
    };
    const char *avutil_names[] = {
        "libavutil." AV_STRINGIFY(LIBAVUTIL_VERSION_MAJOR) ".dylib",
        "libavutil.60.dylib", "libavutil.59.dylib", "libavutil.58.dylib",
        "libavutil.57.dylib", "libavutil.dylib"
    };
    const char *swresample_names[] = {
        "libswresample." AV_STRINGIFY(LIBSWRESAMPLE_VERSION_MAJOR) ".dylib",
        "libswresample.6.dylib", "libswresample.5.dylib", "libswresample.4.dylib",
        "libswresample.3.dylib", "libswresample.dylib"
    };
    const char *swscale_names[] = {
        "libswscale." AV_STRINGIFY(LIBSWSCALE_VERSION_MAJOR) ".dylib",
        "libswscale.9.dylib", "libswscale.8.dylib", "libswscale.7.dylib",
        "libswscale.6.dylib", "libswscale.dylib"
    };
#else
    const char *avformat_names[] = {
        "libavformat.so." AV_STRINGIFY(LIBAVFORMAT_VERSION_MAJOR),
        "libavformat.so.62", "libavformat.so.61", "libavformat.so.60",
        "libavformat.so.59", "libavformat.so"
    };
    const char *avcodec_names[] = {
        "libavcodec.so." AV_STRINGIFY(LIBAVCODEC_VERSION_MAJOR),
        "libavcodec.so.62", "libavcodec.so.61", "libavcodec.so.60",
        "libavcodec.so.59", "libavcodec.so"
    };
    const char *avutil_names[] = {
        "libavutil.so." AV_STRINGIFY(LIBAVUTIL_VERSION_MAJOR),
        "libavutil.so.60", "libavutil.so.59", "libavutil.so.58",
        "libavutil.so.57", "libavutil.so"
    };
    const char *swresample_names[] = {
        "libswresample.so." AV_STRINGIFY(LIBSWRESAMPLE_VERSION_MAJOR),
        "libswresample.so.6", "libswresample.so.5", "libswresample.so.4",
        "libswresample.so.3", "libswresample.so"
    };
    const char *swscale_names[] = {
        "libswscale.so." AV_STRINGIFY(LIBSWSCALE_VERSION_MAJOR),
        "libswscale.so.9", "libswscale.so.8", "libswscale.so.7",
        "libswscale.so.6", "libswscale.so"
    };
#endif

    g_handle_avformat   = try_open(avformat_names,
                                   (int)(sizeof(avformat_names)   / sizeof(avformat_names[0])));
    g_handle_avcodec    = try_open(avcodec_names,
                                   (int)(sizeof(avcodec_names)    / sizeof(avcodec_names[0])));
    g_handle_avutil     = try_open(avutil_names,
                                   (int)(sizeof(avutil_names)     / sizeof(avutil_names[0])));
    g_handle_swresample = try_open(swresample_names,
                                   (int)(sizeof(swresample_names) / sizeof(swresample_names[0])));
    g_handle_swscale    = try_open(swscale_names,
                                   (int)(sizeof(swscale_names)    / sizeof(swscale_names[0])));

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
    LOAD_SYM(g_handle_avcodec, out_funcs, av_packet_move_ref);

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
    if (g_handle_swscale)    { lib_close(g_handle_swscale);    g_handle_swscale    = nullptr; }
    if (g_handle_swresample) { lib_close(g_handle_swresample); g_handle_swresample = nullptr; }
    if (g_handle_avcodec)    { lib_close(g_handle_avcodec);    g_handle_avcodec    = nullptr; }
    if (g_handle_avformat)   { lib_close(g_handle_avformat);   g_handle_avformat   = nullptr; }
    if (g_handle_avutil)     { lib_close(g_handle_avutil);     g_handle_avutil     = nullptr; }
}
