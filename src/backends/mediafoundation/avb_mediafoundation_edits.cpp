#include "avb_mediafoundation_edits.hpp"

#ifdef _WIN32

#include "avb_mediafoundation_common.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr uint32_t box_type(const char (&s)[5]) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(s[0])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(s[1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(s[2])) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(s[3]));
}

uint32_t read_be32(const unsigned char *p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint64_t read_be64(const unsigned char *p) {
    return (static_cast<uint64_t>(read_be32(p)) << 32) | read_be32(p + 4);
}

// Calls f(type, payload, payload_size) for each box in [data, data + size).
// Stops at the first malformed header rather than guessing past it.
template <typename F>
void for_each_box(const unsigned char *data, size_t size, F &&f) {
    size_t offset = 0;
    while (offset + 8 <= size) {
        uint64_t box_size = read_be32(data + offset);
        const uint32_t type = read_be32(data + offset + 4);
        size_t header = 8;
        if (box_size == 1) {
            if (offset + 16 > size) return;
            box_size = read_be64(data + offset + 8);
            header = 16;
        } else if (box_size == 0) {
            box_size = size - offset;
        }
        if (box_size < header || box_size > size - offset) return;
        f(type, data + header + offset, static_cast<size_t>(box_size) - header);
        offset += static_cast<size_t>(box_size);
    }
}

// The timescale of an mvhd or mdhd payload; 0 when it is too short.
uint32_t header_timescale(const unsigned char *p, size_t size) {
    // version/flags, then creation and modification times: 32-bit in version
    // 0, 64-bit in version 1.
    const size_t offset = size > 0 && p[0] == 1 ? 20 : 12;
    return size >= offset + 4 ? read_be32(p + offset) : 0;
}

struct TrackBoxes {
    uint32_t handler = 0;
    uint32_t media_timescale = 0;
    bool has_edits = false;
    double empty_ticks = 0.0;   // movie timescale
    int64_t media_time = 0;     // media timescale
    int64_t first_cts = 0;      // media timescale
};

void read_elst(const unsigned char *p, size_t size, TrackBoxes &track) {
    if (size < 8) return;
    const bool wide = p[0] == 1;
    const uint32_t count = read_be32(p + 4);
    const size_t entry = wide ? 20 : 12;
    double empty = 0.0;
    for (uint32_t i = 0; i < count && 8 + (i + 1) * entry <= size; ++i) {
        const unsigned char *e = p + 8 + i * entry;
        const uint64_t duration = wide ? read_be64(e) : read_be32(e);
        const int64_t media_time =
            wide ? static_cast<int64_t>(read_be64(e + 8))
                 : static_cast<int64_t>(static_cast<int32_t>(read_be32(e + 4)));
        // An empty edit (-1) holds the track back; the first real one says
        // where in the media presentation begins. Later ones -- cuts within
        // the track -- are not followed.
        if (media_time == -1) {
            empty += static_cast<double>(duration);
            continue;
        }
        track.has_edits = true;
        track.empty_ticks = empty;
        track.media_time = media_time;
        return;
    }
}

void read_ctts(const unsigned char *p, size_t size, TrackBoxes &track) {
    if (size < 16 || read_be32(p + 4) == 0) return;
    const uint32_t offset = read_be32(p + 12);
    track.first_cts = p[0] == 1
        ? static_cast<int64_t>(static_cast<int32_t>(offset))
        : static_cast<int64_t>(offset);
}

void read_trak(const unsigned char *p, size_t size, TrackBoxes &track) {
    for_each_box(p, size, [&](uint32_t type, const unsigned char *b, size_t n) {
        if (type == box_type("edts")) {
            for_each_box(b, n, [&](uint32_t t, const unsigned char *e, size_t m) {
                if (t == box_type("elst")) read_elst(e, m, track);
            });
        } else if (type == box_type("mdia")) {
            for_each_box(b, n, [&](uint32_t t, const unsigned char *m, size_t k) {
                if (t == box_type("mdhd")) {
                    track.media_timescale = header_timescale(m, k);
                } else if (t == box_type("hdlr")) {
                    if (k >= 12) track.handler = read_be32(m + 8);
                } else if (t == box_type("minf")) {
                    for_each_box(m, k, [&](uint32_t u, const unsigned char *s, size_t j) {
                        if (u != box_type("stbl")) return;
                        for_each_box(s, j, [&](uint32_t v, const unsigned char *c, size_t i) {
                            if (v == box_type("ctts")) read_ctts(c, i, track);
                        });
                    });
                }
            });
        }
    });
}

} // namespace

std::vector<MfTrackEdit> mf_parse_moov_edits(const unsigned char *moov,
                                             size_t size) {
    uint32_t movie_timescale = 0;
    std::vector<TrackBoxes> tracks;
    for_each_box(moov, size, [&](uint32_t type, const unsigned char *p, size_t n) {
        if (type == box_type("mvhd")) {
            movie_timescale = header_timescale(p, n);
        } else if (type == box_type("trak")) {
            TrackBoxes track;
            read_trak(p, n, track);
            tracks.push_back(track);
        }
    });

    std::vector<MfTrackEdit> edits;
    for (const auto &track : tracks) {
        const bool video = track.handler == box_type("vide");
        if (!video && track.handler != box_type("soun")) continue;
        MfTrackEdit edit;
        edit.video = video;
        if (track.media_timescale > 0) {
            const double scale = static_cast<double>(track.media_timescale);
            edit.first_cts_sec = static_cast<double>(track.first_cts) / scale;
            if (track.has_edits) {
                edit.shift_sec = static_cast<double>(track.media_time) / scale;
                if (movie_timescale > 0)
                    edit.shift_sec -= track.empty_ticks / movie_timescale;
            }
        }
        edits.push_back(edit);
    }
    return edits;
}

std::vector<MfTrackEdit> mf_read_track_edits(const char *path) {
    FILE *file = mf_fopen_utf8(path, L"rb");
    if (!file) return {};

    // The moov can come after the media data (a file written without
    // faststart), so the top level is walked by its headers alone.
    std::vector<unsigned char> moov;
    int64_t offset = 0;
    for (;;) {
        unsigned char header[16];
        if (_fseeki64(file, offset, SEEK_SET) != 0 ||
            fread(header, 1, 8, file) != 8)
            break;
        uint64_t box_size = read_be32(header);
        const uint32_t type = read_be32(header + 4);
        int64_t header_size = 8;
        if (box_size == 1) {
            if (fread(header + 8, 1, 8, file) != 8) break;
            box_size = read_be64(header + 8);
            header_size = 16;
        } else if (box_size == 0) {
            break; // runs to the end of the file: nothing after it
        }
        if (box_size < static_cast<uint64_t>(header_size)) break;
        if (type == box_type("moov")) {
            // A moov is index tables, megabytes at the very most; anything
            // claiming more is not one to trust.
            const uint64_t payload = box_size - header_size;
            if (payload > (256ull << 20)) break;
            moov.resize(static_cast<size_t>(payload));
            if (fread(moov.data(), 1, moov.size(), file) != moov.size())
                moov.clear();
            break;
        }
        offset += static_cast<int64_t>(box_size);
    }
    fclose(file);
    if (moov.empty()) return {};
    return mf_parse_moov_edits(moov.data(), moov.size());
}

#endif
