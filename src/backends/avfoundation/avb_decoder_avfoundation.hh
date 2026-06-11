#pragma once

#include "../../avb_decoder_impl.hpp"
#include <string>

class AvbDecoderAVFoundation : public AvbDecoderImpl {
public:
    AvbDecoderAVFoundation();
    ~AvbDecoderAVFoundation() override;

    avb_result open_file(const char *path, const avb_decode_options &options) override;
    avb_result get_media_info(avb_media_info &out_info) override;
    avb_result seek(double seconds) override;
    int read_audio_f32(float *dst_interleaved, int frames) override;
    double audio_next_pts() override;
    avb_result read_video_frame(avb_video_frame &out_frame) override;
    void release_video_frame(avb_video_frame &frame) override;
    const char *get_last_error() const override;
    const char *get_backend_name() const override;

private:
    struct Impl;
    bool fill_audio_buffer();
    avb_result open_custom_video_decoder(void *track, const avb_decode_options &options);
    avb_result read_custom_video_frame(avb_video_frame &out_frame);

    Impl *m_impl = nullptr;
    std::string m_last_error;
};
