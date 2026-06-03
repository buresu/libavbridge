#include "avb_gstreamer_loader.hpp"

#include <dlfcn.h>
#include <cstdio>

static void *g_handle_gst     = nullptr;
static void *g_handle_gstapp  = nullptr;
static void *g_handle_pbutils = nullptr;
static void *g_handle_gobject = nullptr;
static void *g_handle_glib    = nullptr;

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
    } while (0)

bool avb_gst_load(AvbGstFuncs &out_funcs, char *err_buf, int err_buf_size) {
    const char *gst_names[]     = { "libgstreamer-1.0.so.0", "libgstreamer-1.0.so" };
    const char *gstapp_names[]  = { "libgstapp-1.0.so.0",    "libgstapp-1.0.so" };
    const char *pbutils_names[] = { "libgstpbutils-1.0.so.0","libgstpbutils-1.0.so" };
    const char *gobject_names[] = { "libgobject-2.0.so.0",   "libgobject-2.0.so" };
    const char *glib_names[]    = { "libglib-2.0.so.0",      "libglib-2.0.so" };

    g_handle_gst     = try_open(gst_names,     2);
    g_handle_gstapp  = try_open(gstapp_names,  2);
    g_handle_pbutils = try_open(pbutils_names, 2);
    g_handle_gobject = try_open(gobject_names, 2);
    g_handle_glib    = try_open(glib_names,    2);

    if (!g_handle_gst || !g_handle_gstapp || !g_handle_pbutils ||
        !g_handle_gobject || !g_handle_glib) {
        snprintf(err_buf, err_buf_size,
            "GStreamer backend unavailable: libgstreamer-1.0/libgstapp-1.0/"
            "libgstpbutils-1.0/libgobject-2.0/libglib-2.0 could not be loaded. "
            "Install GStreamer runtime libraries and try again.");
        avb_gst_unload();
        return false;
    }

    LOAD_SYM(g_handle_gst, out_funcs, gst_init);
    LOAD_SYM(g_handle_gst, out_funcs, gst_element_factory_make);
    LOAD_SYM(g_handle_gst, out_funcs, gst_parse_bin_from_description);
    LOAD_SYM(g_handle_gst, out_funcs, gst_element_set_state);
    LOAD_SYM(g_handle_gst, out_funcs, gst_element_get_state);
    LOAD_SYM(g_handle_gst, out_funcs, gst_element_query_duration);
    LOAD_SYM(g_handle_gst, out_funcs, gst_element_seek_simple);
    LOAD_SYM(g_handle_gst, out_funcs, gst_bin_get_by_name);
    LOAD_SYM(g_handle_gst, out_funcs, gst_caps_get_structure);
    LOAD_SYM(g_handle_gst, out_funcs, gst_structure_get_name);
    LOAD_SYM(g_handle_gst, out_funcs, gst_structure_get_int);
    LOAD_SYM(g_handle_gst, out_funcs, gst_structure_get_fraction);
    LOAD_SYM(g_handle_gst, out_funcs, gst_sample_get_buffer);
    LOAD_SYM(g_handle_gst, out_funcs, gst_sample_get_caps);
    LOAD_SYM(g_handle_gst, out_funcs, gst_buffer_map);
    LOAD_SYM(g_handle_gst, out_funcs, gst_buffer_unmap);
    LOAD_SYM(g_handle_gst, out_funcs, gst_buffer_get_size);
    LOAD_SYM(g_handle_gst, out_funcs, gst_mini_object_unref);
    LOAD_SYM(g_handle_gst, out_funcs, gst_object_unref);
    LOAD_SYM(g_handle_gst, out_funcs, gst_filename_to_uri);

    LOAD_SYM(g_handle_gstapp, out_funcs, gst_app_sink_pull_sample);
    LOAD_SYM(g_handle_gstapp, out_funcs, gst_app_sink_try_pull_preroll);
    LOAD_SYM(g_handle_gstapp, out_funcs, gst_app_sink_set_max_buffers);
    LOAD_SYM(g_handle_gstapp, out_funcs, gst_app_sink_set_drop);

    LOAD_SYM(g_handle_pbutils, out_funcs, gst_discoverer_new);
    LOAD_SYM(g_handle_pbutils, out_funcs, gst_discoverer_discover_uri);
    LOAD_SYM(g_handle_pbutils, out_funcs, gst_discoverer_info_get_audio_streams);
    LOAD_SYM(g_handle_pbutils, out_funcs, gst_discoverer_info_get_video_streams);
    LOAD_SYM(g_handle_pbutils, out_funcs, gst_discoverer_stream_info_get_caps);
    LOAD_SYM(g_handle_pbutils, out_funcs, gst_discoverer_stream_info_list_free);

    LOAD_SYM(g_handle_gobject, out_funcs, g_object_set);
    LOAD_SYM(g_handle_gobject, out_funcs, g_object_unref);

    LOAD_SYM(g_handle_glib, out_funcs, g_free);
    LOAD_SYM(g_handle_glib, out_funcs, g_clear_error);

    return true;
}

void avb_gst_unload() {
    // Note: gst_init is not paired with gst_deinit here; the process may keep
    // GStreamer initialised for its lifetime, which is the common pattern.
    if (g_handle_pbutils) { dlclose(g_handle_pbutils); g_handle_pbutils = nullptr; }
    if (g_handle_gstapp)  { dlclose(g_handle_gstapp);  g_handle_gstapp  = nullptr; }
    if (g_handle_gst)     { dlclose(g_handle_gst);     g_handle_gst     = nullptr; }
    if (g_handle_gobject) { dlclose(g_handle_gobject); g_handle_gobject = nullptr; }
    if (g_handle_glib)    { dlclose(g_handle_glib);    g_handle_glib    = nullptr; }
}
