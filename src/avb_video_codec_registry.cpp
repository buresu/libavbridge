#include "avb_video_codec_registry.hpp"

#include <algorithm>
#include <mutex>
#include <vector>

namespace {

std::mutex &registry_mutex() {
    static std::mutex m;
    return m;
}

std::vector<const avb_video_decoder_plugin *> &registry_plugins() {
    static std::vector<const avb_video_decoder_plugin *> plugins;
    return plugins;
}

std::vector<const avb_video_encoder_plugin *> &encoder_plugins() {
    static std::vector<const avb_video_encoder_plugin *> plugins;
    return plugins;
}

bool plugin_valid(const avb_video_decoder_plugin *plugin) {
    return plugin &&
           plugin->struct_size >= sizeof(avb_video_decoder_plugin) &&
           plugin->can_decode &&
           plugin->open &&
           plugin->decode_packet;
}

bool plugin_valid(const avb_video_encoder_plugin *plugin) {
    return plugin &&
           plugin->struct_size >= sizeof(avb_video_encoder_plugin) &&
           plugin->can_encode &&
           plugin->open &&
           plugin->encode_frame;
}

} // namespace

const avb_video_decoder_plugin *avb_find_video_decoder_plugin(
    const avb_video_stream_info &stream,
    const avb_decode_options &options
) {
    if (!options.enable_custom_video_decoders) return nullptr;

    std::vector<const avb_video_decoder_plugin *> plugins;
    {
        std::lock_guard<std::mutex> lock(registry_mutex());
        plugins = registry_plugins();
    }
    for (const avb_video_decoder_plugin *plugin : plugins) {
        if (plugin && plugin->can_decode(&stream, &options)) return plugin;
    }
    return nullptr;
}

const avb_video_encoder_plugin *avb_find_video_encoder_plugin(
    const avb_video_encode_info &info
) {
    std::vector<const avb_video_encoder_plugin *> plugins;
    {
        std::lock_guard<std::mutex> lock(registry_mutex());
        plugins = encoder_plugins();
    }
    for (const avb_video_encoder_plugin *plugin : plugins) {
        if (plugin && plugin->can_encode(&info)) return plugin;
    }
    return nullptr;
}

extern "C" {

avb_result avb_register_video_decoder(const avb_video_decoder_plugin *plugin) {
    if (!plugin_valid(plugin)) return AVB_ERROR_INVALID_ARGUMENT;

    std::lock_guard<std::mutex> lock(registry_mutex());
    auto &plugins = registry_plugins();
    auto it = std::find(plugins.begin(), plugins.end(), plugin);
    if (it == plugins.end()) plugins.push_back(plugin);
    return AVB_OK;
}

avb_result avb_unregister_video_decoder(const avb_video_decoder_plugin *plugin) {
    if (!plugin) return AVB_ERROR_INVALID_ARGUMENT;

    std::lock_guard<std::mutex> lock(registry_mutex());
    auto &plugins = registry_plugins();
    auto old_size = plugins.size();
    plugins.erase(std::remove(plugins.begin(), plugins.end(), plugin), plugins.end());
    return plugins.size() == old_size ? AVB_ERROR_INVALID_ARGUMENT : AVB_OK;
}

avb_result avb_register_video_encoder(const avb_video_encoder_plugin *plugin) {
    if (!plugin_valid(plugin)) return AVB_ERROR_INVALID_ARGUMENT;

    std::lock_guard<std::mutex> lock(registry_mutex());
    auto &plugins = encoder_plugins();
    auto it = std::find(plugins.begin(), plugins.end(), plugin);
    if (it == plugins.end()) plugins.push_back(plugin);
    return AVB_OK;
}

avb_result avb_unregister_video_encoder(const avb_video_encoder_plugin *plugin) {
    if (!plugin) return AVB_ERROR_INVALID_ARGUMENT;

    std::lock_guard<std::mutex> lock(registry_mutex());
    auto &plugins = encoder_plugins();
    auto old_size = plugins.size();
    plugins.erase(std::remove(plugins.begin(), plugins.end(), plugin), plugins.end());
    return plugins.size() == old_size ? AVB_ERROR_INVALID_ARGUMENT : AVB_OK;
}

} // extern "C"
