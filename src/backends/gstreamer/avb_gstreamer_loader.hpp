#pragma once

extern "C" {
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/pbutils/pbutils.h>
}

#include <stdbool.h>

// GLib defines g_free as a function-like macro (sized free). Undefine it so the
// identifier refers to our function pointer / the real dlsym'd symbol instead.
#ifdef g_free
#undef g_free
#endif

// Pointers to dynamically loaded GStreamer / GLib / GObject functions.
//
// GStreamer is loaded at runtime via dlopen/dlsym (never linked at build time),
// mirroring the ffmpeg backend. The real headers are used at build time only,
// so the struct layouts and enum/macro values this code relies on (GstMapInfo,
// GstBuffer::pts, GST_SECOND, GST_FORMAT_TIME, ...) match the loaded library.
struct AvbGstFuncs {
    // libgstreamer-1.0
    void (*gst_init)(int *, char ***);
    GstElement *(*gst_element_factory_make)(const gchar *, const gchar *);
    GstElement *(*gst_parse_bin_from_description)(const gchar *, gboolean, GError **);
    GstStateChangeReturn (*gst_element_set_state)(GstElement *, GstState);
    GstStateChangeReturn (*gst_element_get_state)(GstElement *, GstState *, GstState *, GstClockTime);
    gboolean (*gst_element_query_duration)(GstElement *, GstFormat, gint64 *);
    gboolean (*gst_element_seek_simple)(GstElement *, GstFormat, GstSeekFlags, gint64);
    GstElement *(*gst_bin_get_by_name)(GstBin *, const gchar *);
    GstStructure *(*gst_caps_get_structure)(const GstCaps *, guint);
    const gchar *(*gst_structure_get_name)(const GstStructure *);
    gboolean (*gst_structure_get_int)(const GstStructure *, const gchar *, gint *);
    gboolean (*gst_structure_get_fraction)(const GstStructure *, const gchar *, gint *, gint *);
    GstBuffer *(*gst_sample_get_buffer)(GstSample *);
    GstCaps *(*gst_sample_get_caps)(GstSample *);
    gboolean (*gst_buffer_map)(GstBuffer *, GstMapInfo *, GstMapFlags);
    void (*gst_buffer_unmap)(GstBuffer *, GstMapInfo *);
    gsize (*gst_buffer_get_size)(GstBuffer *);
    void (*gst_mini_object_unref)(GstMiniObject *);
    void (*gst_object_unref)(gpointer);
    gchar *(*gst_filename_to_uri)(const gchar *, GError **);

    // Encoding (appsrc -> encoders -> muxer -> filesink)
    GstElement *(*gst_parse_launch)(const gchar *, GError **);
    GstCaps *(*gst_caps_from_string)(const gchar *);
    GstBuffer *(*gst_buffer_new_allocate)(GstAllocator *, gsize, GstAllocationParams *);
    gsize (*gst_buffer_fill)(GstBuffer *, gsize, gconstpointer, gsize);
    GstBus *(*gst_element_get_bus)(GstElement *);
    GstMessage *(*gst_bus_timed_pop_filtered)(GstBus *, GstClockTime, GstMessageType);
    void (*gst_message_parse_error)(GstMessage *, GError **, gchar **);

    // libgstapp-1.0
    GstSample *(*gst_app_sink_pull_sample)(GstAppSink *);
    GstSample *(*gst_app_sink_try_pull_preroll)(GstAppSink *, GstClockTime);
    void (*gst_app_sink_set_max_buffers)(GstAppSink *, guint);
    void (*gst_app_sink_set_drop)(GstAppSink *, gboolean);
    GstFlowReturn (*gst_app_src_push_buffer)(GstAppSrc *, GstBuffer *);
    GstFlowReturn (*gst_app_src_end_of_stream)(GstAppSrc *);

    // libgstpbutils-1.0 (source codec discovery)
    GstDiscoverer *(*gst_discoverer_new)(GstClockTime, GError **);
    GstDiscovererInfo *(*gst_discoverer_discover_uri)(GstDiscoverer *, const gchar *, GError **);
    GList *(*gst_discoverer_info_get_audio_streams)(GstDiscovererInfo *);
    GList *(*gst_discoverer_info_get_video_streams)(GstDiscovererInfo *);
    GstCaps *(*gst_discoverer_stream_info_get_caps)(GstDiscovererStreamInfo *);
    void (*gst_discoverer_stream_info_list_free)(GList *);

    // libgobject-2.0 (variadic property setter/getter)
    void (*g_object_set)(gpointer, const gchar *, ...);
    void (*g_object_get)(gpointer, const gchar *, ...);
    void (*g_object_unref)(gpointer);

    // libglib-2.0
    void (*g_free)(gpointer);
    void (*g_clear_error)(GError **);
    void (*g_error_free)(GError *);
};

bool avb_gst_load(AvbGstFuncs &out_funcs, char *err_buf, int err_buf_size);
void avb_gst_unload();
