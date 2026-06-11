#pragma once

#include "avbridge.h"

#include <string>

class AvbDecoderImpl {
public:
    virtual ~AvbDecoderImpl();

    virtual avb_result open_file(const char *path, const avb_decode_options &options) = 0;

    // Open from custom I/O callbacks. The default implementation buffers the
    // whole stream into a temporary file and calls open_file(); implementations
    // that can consume callbacks natively (FFmpeg) override this. The temp file,
    // if created, is removed by ~AvbDecoderImpl.
    virtual avb_result open_io(const avb_io_callbacks &cb, void *user,
                               const avb_decode_options &options);

    virtual avb_result get_media_info(avb_media_info &out_info) = 0;
    virtual avb_result seek(double seconds) = 0;
    virtual int read_audio_f32(float *dst_interleaved, int frames) = 0;

    // Presentation time (seconds) of the next audio sample that read_audio_f32
    // would return, or -1 if unknown. May fill internal buffers as a side
    // effect. Default: unknown.
    virtual double audio_next_pts() { return -1.0; }

    virtual avb_result read_video_frame(avb_video_frame &out_frame) = 0;
    virtual void release_video_frame(avb_video_frame &frame) = 0;
    virtual const char *get_last_error() const = 0;
    virtual const char *get_backend_name() const = 0;

protected:
    // Path of a temp file created by the default open_io(), removed on destroy.
    std::string m_spill_path;
};

AvbDecoderImpl *avb_create_decoder_impl(avb_backend backend);
