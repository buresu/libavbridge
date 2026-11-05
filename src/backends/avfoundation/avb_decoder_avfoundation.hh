#pragma once

#include "avb_decoder_impl.hpp"
#include <string>
#include <vector>

class AvbDecoderAVFoundation : public AvbDecoderImpl {
public:
    AvbDecoderAVFoundation();
    ~AvbDecoderAVFoundation() override;

    avb_result open_file(const char *path, const avb_decode_options &options) override;
    avb_result get_media_info(avb_media_info &out_info) override;
    avb_result seek(double seconds) override;
    int read_audio_f32(float *dst_interleaved, int frames) override;
    double audio_next_pts() override;
    bool audio_read_failed() const override;
    avb_result read_video_frame(avb_video_frame &out_frame) override;
    void release_video_frame(avb_video_frame &frame) override;
    const char *get_last_error() const override;
    const char *get_backend_name() const override;

private:
    struct Impl;
    // The public entry points run these behind an Objective-C exception guard:
    // AVAssetReader raises NSExceptions instead of returning errors for some
    // misuse, and one escaping through the C API takes the process down.
    avb_result open_file_impl(const char *path, const avb_decode_options &options);
    avb_result seek_impl(double seconds);
    avb_result read_video_frame_impl(avb_video_frame &out_frame);

    bool fill_audio_buffer();
    // The reader's next block of interleaved float audio at its output rate.
    // False at end of stream, or on failure (audio_read_failed() then says so).
    bool copy_reader_audio(std::vector<float> &out, double &pts);
    // Fill the audio buffer through the AudioConverter resampler.
    bool fill_resampled_audio();
    // At a NULL sample buffer: true (with the reader's error recorded) when the
    // reader failed rather than reaching the end of the stream.
    bool reader_failed();
    avb_result open_custom_video_decoder(void *track, const avb_decode_options &options);
    avb_result read_custom_video_frame(avb_video_frame &out_frame);

    Impl *m_impl = nullptr;
    std::string m_last_error;
};
