#pragma once

#ifdef _WIN32

#include <cstddef>
#include <vector>

// Where an MP4/MOV track's samples sit on the presentation timeline, as its
// edit list (moov/trak/edts/elst) puts them. An encoder that reorders frames
// (H.264 with B-frames) starts the picture a few frames into the track's media
// time, and AAC starts past its encoder delay; the edit list moves both back
// to 0. FFmpeg and AVFoundation apply it; Media Foundation's MPEG-4 source
// does not, at least on Windows 11 builds from October 2026 (26100.9711): it
// stamps samples with their media time, the picture two frames late and the
// sound by its encoder delay.
struct MfTrackEdit {
    bool video = false;          // a 'vide' track; otherwise 'soun'
    // Presentation time = media time - shift_sec.
    double shift_sec = 0.0;
    // Media (composition) time of the first sample in decode order: what an
    // untouched source stamps the first packet it reads.
    double first_cts_sec = 0.0;
};

// The video and sound tracks of an ISO base media file (MP4, MOV), in file
// order. Empty for anything else, or a file whose moov cannot be read.
std::vector<MfTrackEdit> mf_read_track_edits(const char *path);

// The same, from a moov box's payload (what follows its 8-byte header).
std::vector<MfTrackEdit> mf_parse_moov_edits(const unsigned char *moov,
                                             size_t size);

#endif
