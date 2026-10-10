// Inject a real asynchronous bus error into a decoder pipeline. A passive
// GStreamer tracer locates the pipeline without adding a production test API.
#include "test.hpp"

#include <gst/gst.h>

#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

static std::mutex pipelines_mutex;
static std::vector<GstElement *> pipelines;

typedef struct { GstTracer parent; } AvbErrorTracer;
typedef struct { GstTracerClass parent; } AvbErrorTracerClass;
G_DEFINE_TYPE(AvbErrorTracer, avb_error_tracer, GST_TYPE_TRACER)

static void element_new(GstTracer *, GstClockTime, GstElement *element) {
    if (!GST_IS_PIPELINE(element)) return;
    std::lock_guard<std::mutex> lock(pipelines_mutex);
    pipelines.push_back(GST_ELEMENT(gst_object_ref(element)));
}
static void avb_error_tracer_class_init(AvbErrorTracerClass *) {}
static void avb_error_tracer_init(AvbErrorTracer *tracer) {
    gst_tracing_register_hook(GST_TRACER(tracer), "element-new", G_CALLBACK(element_new));
}

static unsigned char block[8]{};
static int can_decode(const avb_video_stream_info *, const avb_decode_options *options) {
    return options->video_format == AVB_PIXEL_FORMAT_BC1_RGBA;
}
static avb_result open_plugin(void **ctx, const avb_video_stream_info *, const avb_decode_options *) {
    *ctx = block;
    return AVB_OK;
}
static avb_result decode_packet(void *, const avb_encoded_packet *packet, avb_video_frame *frame) {
    *frame = {};
    frame->pts_sec = packet->pts_sec;
    frame->format = AVB_PIXEL_FORMAT_BC1_RGBA;
    frame->data = block;
    frame->data_size = sizeof(block);
    return AVB_OK;
}
static void release_frame(void *, avb_video_frame *) {}
static void close_plugin(void *) {}

static void check_error(avb::test::Context &test, const char *path, bool audio, bool custom) {
    avb_decode_options options = avb_decode_options_default();
    options.backend = AVB_BACKEND_GSTREAMER;
    options.hardware_policy = AVB_HARDWARE_DISABLED;
    options.enable_video = !audio || custom;
    options.enable_audio = audio;
    options.video_format = custom ? AVB_PIXEL_FORMAT_BC1_RGBA : AVB_PIXEL_FORMAT_BGRA8;
    avb_decoder *decoder = nullptr;
    const avb_result opened = avb_decoder_open(&decoder, path, &options);
    test.equal(opened, AVB_OK, "decoder opens before bus failure");
    GstElement *pipeline = nullptr;
    {
        std::lock_guard<std::mutex> lock(pipelines_mutex);
        for (GstElement *candidate : pipelines) {
            GstElement *sink = gst_bin_get_by_name(GST_BIN(candidate), audio ? "avb_asink" : "avb_vsink");
            if (sink) { pipeline = candidate; gst_object_unref(sink); break; }
        }
    }
    test.check(pipeline != nullptr, "tracer locates the decoder pipeline");
    if (opened == AVB_OK && pipeline) {
        // In PAUSED, appsink cannot produce new samples. Post ERROR later,
        // without EOS, to exercise the wait path as well as error reporting.
        gst_element_set_state(pipeline, GST_STATE_PAUSED);
        auto post_failure = [pipeline] {
            GError *error = g_error_new_literal(GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_READ,
                                               "avbridge injected read failure");
            GstMessage *message = gst_message_new_error(GST_OBJECT(pipeline), error, nullptr);
            g_error_free(error);
            gst_element_post_message(pipeline, message);
        };
        std::thread fail;
        // The unbounded audio-only sink can already hold the complete stream.
        // Inject before draining it; video exercises the delayed wait case.
        if (audio) post_failure();
        else fail = std::thread([post_failure] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            post_failure();
        });
        avb_result result = AVB_OK;
        std::vector<float> samples(4096 * 2);
        for (int i = 0; i < 1000; ++i) {
            if (audio) {
                if (avb_decoder_read_audio_f32(decoder, samples.data(), 4096, nullptr) == 0) break;
            } else {
                avb_video_frame frame{};
                result = avb_decoder_read_video_frame(decoder, &frame);
                if (result != AVB_OK) break;
                avb_decoder_release_video_frame(decoder, &frame);
            }
        }
        if (fail.joinable()) fail.join();
        if (audio) {
            test.equal(avb_decoder_audio_at_eof(decoder), 0, "bus error does not become audio EOF");
            test.equal(avb_decoder_read_audio_f32(decoder, samples.data(), 4096, nullptr), 0,
                       "repeated audio read remains stopped");
            test.equal(avb_decoder_audio_at_eof(decoder), 0, "repeated audio failure is not EOF");
        } else {
            test.equal(result, AVB_ERROR_DECODE_FAILED, "bus error becomes DECODE_FAILED");
            avb_video_frame frame{};
            test.equal(avb_decoder_read_video_frame(decoder, &frame), AVB_ERROR_DECODE_FAILED,
                       "repeated video read preserves failure");
        }
        const char *error = avb_decoder_get_last_error(decoder);
        test.check(error && std::strstr(error, "avbridge injected read failure"),
                   "original bus diagnostic reaches the public API");
    }
    avb_decoder_close(decoder);
    std::vector<GstElement *> old;
    { std::lock_guard<std::mutex> lock(pipelines_mutex); old.swap(pipelines); }
    for (GstElement *pipeline_ref : old) gst_object_unref(pipeline_ref);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    gst_init(nullptr, nullptr);
    GstTracer *tracer = GST_TRACER(g_object_new(avb_error_tracer_get_type(), nullptr));
    avb_video_decoder_plugin plugin{};
    plugin.struct_size = sizeof(plugin);
    plugin.name = "bus-error-packet-decoder";
    plugin.can_decode = can_decode;
    plugin.open = open_plugin;
    plugin.decode_packet = decode_packet;
    plugin.release_frame = release_frame;
    plugin.close = close_plugin;
    avb_register_video_decoder(&plugin);
    avb::test::Context test;
    for (bool custom : {false, true})
        for (bool audio : {false, true}) check_error(test, argv[1], audio, custom);
    avb_unregister_video_decoder(&plugin);
    gst_object_unref(tracer);
    gst_deinit();
    return test.finish("GStreamer read failures");
}
