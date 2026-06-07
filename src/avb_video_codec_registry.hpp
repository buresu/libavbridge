#pragma once

#include "avbridge.h"

const avb_video_decoder_plugin *avb_find_video_decoder_plugin(
    const avb_video_stream_info &stream,
    const avb_decode_options &options
);

const avb_video_encoder_plugin *avb_find_video_encoder_plugin(
    const avb_video_encode_info &info
);
