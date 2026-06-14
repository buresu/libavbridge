#include "avb_video_plugins.hpp"

#include <algorithm>
#include <mutex>
#include <vector>

namespace {

template <typename Plugin>
class PluginRegistry {
public:
    avb_result add(const Plugin *plugin) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = std::find(m_plugins.begin(), m_plugins.end(), plugin);
        if (it == m_plugins.end()) m_plugins.push_back(plugin);
        return AVB_OK;
    }

    avb_result remove(const Plugin *plugin) {
        if (!plugin) return AVB_ERROR_INVALID_ARGUMENT;

        std::lock_guard<std::mutex> lock(m_mutex);
        auto old_size = m_plugins.size();
        m_plugins.erase(
            std::remove(m_plugins.begin(), m_plugins.end(), plugin),
            m_plugins.end());
        return m_plugins.size() == old_size
            ? AVB_ERROR_INVALID_ARGUMENT
            : AVB_OK;
    }

    std::vector<const Plugin *> snapshot() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_plugins;
    }

private:
    std::mutex m_mutex;
    std::vector<const Plugin *> m_plugins;
};

PluginRegistry<avb_video_decoder_plugin> &decoder_plugins() {
    static PluginRegistry<avb_video_decoder_plugin> plugins;
    return plugins;
}

PluginRegistry<avb_video_encoder_plugin> &encoder_plugins() {
    static PluginRegistry<avb_video_encoder_plugin> plugins;
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

    auto plugins = decoder_plugins().snapshot();
    for (const avb_video_decoder_plugin *plugin : plugins) {
        if (plugin && plugin->can_decode(&stream, &options)) return plugin;
    }
    return nullptr;
}

const avb_video_encoder_plugin *avb_find_video_encoder_plugin(
    const avb_video_encode_info &info
) {
    auto plugins = encoder_plugins().snapshot();
    for (const avb_video_encoder_plugin *plugin : plugins) {
        if (plugin && plugin->can_encode(&info)) return plugin;
    }
    return nullptr;
}

extern "C" {

avb_result avb_register_video_decoder(const avb_video_decoder_plugin *plugin) {
    if (!plugin_valid(plugin)) return AVB_ERROR_INVALID_ARGUMENT;
    return decoder_plugins().add(plugin);
}

avb_result avb_unregister_video_decoder(const avb_video_decoder_plugin *plugin) {
    return decoder_plugins().remove(plugin);
}

avb_result avb_register_video_encoder(const avb_video_encoder_plugin *plugin) {
    if (!plugin_valid(plugin)) return AVB_ERROR_INVALID_ARGUMENT;
    return encoder_plugins().add(plugin);
}

avb_result avb_unregister_video_encoder(const avb_video_encoder_plugin *plugin) {
    return encoder_plugins().remove(plugin);
}

} // extern "C"
