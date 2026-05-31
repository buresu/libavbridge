#pragma once

#include "avbridge.h"

class AvbBackend {
public:
    virtual ~AvbBackend() = default;

    virtual avb_result open_file(const char *path, const avb_open_options &options) = 0;
    virtual avb_result get_media_info(avb_media_info &out_info) = 0;
    virtual avb_result seek(double seconds) = 0;
    virtual int read_audio_f32(float *dst_interleaved, int frames) = 0;
    virtual avb_result read_video_frame(avb_video_frame &out_frame) = 0;
    virtual void release_video_frame(avb_video_frame &frame) = 0;
    virtual const char *get_last_error() const = 0;
    virtual const char *get_backend_name() const = 0;
};

AvbBackend *avb_create_backend(avb_backend backend);
