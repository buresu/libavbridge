#pragma once

#include <cstring>
#include <vector>

// Interleaved float32 audio staging buffer shared by the decoder backends.
//
// Both the FFmpeg and GStreamer decoders decode/resample into an interleaved
// float buffer and then hand it out frame-by-frame through read_audio_f32().
// The drain loop (memcpy + head-PTS advance) and the next-sample-PTS query are
// identical; only the refill step is backend-specific, so it is passed in as a
// callable.
//
// `pts` tracks the presentation time (seconds) of the sample currently at
// `pos`, or -1 if unknown. Backends append into `data` from their fill routine
// and stamp `pts` when the buffer was empty (see the *_fill_audio_buffer
// implementations).
struct AvbAudioBuffer {
    std::vector<float> data;
    int    pos = 0;     // read cursor, in floats
    double pts = -1.0;  // presentation time of data[pos], or -1 if unknown

    void clear() { data.clear(); pos = 0; pts = -1.0; }
    bool empty() const { return pos >= (int)data.size(); }

    // Copy up to `frames` interleaved frames into `dst`, refilling via `fill`
    // (a callable returning bool: true if more data was produced) when drained.
    // Returns the number of whole frames written.
    template <class FillFn>
    int read(float *dst, int frames, int channels, int sample_rate, FillFn fill) {
        if (channels <= 0) return 0;
        int samples_needed  = frames * channels;
        int samples_written = 0;

        while (samples_written < samples_needed) {
            int available = (int)data.size() - pos;
            if (available > 0) {
                int to_copy = samples_needed - samples_written;
                if (to_copy > available) to_copy = available;
                std::memcpy(dst + samples_written, data.data() + pos,
                            (size_t)to_copy * sizeof(float));
                pos += to_copy;
                samples_written += to_copy;
                if (pts >= 0.0 && sample_rate > 0)
                    pts += (double)(to_copy / channels) / sample_rate;
                if (empty()) { data.clear(); pos = 0; }
                continue;
            }
            if (!fill()) break;
        }
        return samples_written / channels;
    }

    // Presentation time of the next sample read() would return, refilling once
    // if the buffer is currently drained. Returns -1 if no more data.
    template <class FillFn>
    double next_pts(FillFn fill) {
        if (empty() && !fill()) return -1.0;
        return pts;
    }
};
