#pragma once

#include "avbridge.h"

const avb_video_decoder_plugin *avb_find_video_decoder_plugin(
    const avb_video_stream_info &stream,
    const avb_decode_options &options
);

const avb_video_encoder_plugin *avb_find_video_encoder_plugin(
    const avb_video_encode_info &info
);

class AvbVideoEncoderPacketScope {
public:
    AvbVideoEncoderPacketScope(
        const avb_video_encoder_plugin *plugin,
        void *context,
        avb_encoded_packet &packet)
        : m_plugin(plugin), m_context(context), m_packet(&packet) {}

    ~AvbVideoEncoderPacketScope() {
        if (m_plugin && m_plugin->release_packet && m_packet)
            m_plugin->release_packet(m_context, m_packet);
    }

    AvbVideoEncoderPacketScope(
        const AvbVideoEncoderPacketScope &) = delete;
    AvbVideoEncoderPacketScope &operator=(
        const AvbVideoEncoderPacketScope &) = delete;

private:
    const avb_video_encoder_plugin *m_plugin;
    void *m_context;
    avb_encoded_packet *m_packet;
};
