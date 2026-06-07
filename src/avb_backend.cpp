#include "avb_backend.hpp"

#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <cstdlib>
#  include <unistd.h>
#endif

#if defined(AVB_ENABLE_FFMPEG)
#include "backends/ffmpeg/avb_backend_ffmpeg.hpp"
#endif

#if defined(AVB_ENABLE_MEDIAFOUNDATION)
#include "backends/mediafoundation/avb_backend_mediafoundation.hpp"
#endif

#if defined(AVB_ENABLE_AVFOUNDATION)
#include "backends/avfoundation/avb_backend_avfoundation.hh"
#endif

#if defined(AVB_ENABLE_GSTREAMER)
#include "backends/gstreamer/avb_backend_gstreamer.hpp"
#endif

AvbBackend::~AvbBackend() {
    if (!m_spill_path.empty()) {
        std::remove(m_spill_path.c_str());
    }
}

namespace {

const char *avb_extension_for_magic(const std::vector<unsigned char> &head) {
    if (head.size() >= 12 && std::memcmp(head.data() + 4, "ftyp", 4) == 0) {
        if (std::memcmp(head.data() + 8, "qt  ", 4) == 0) return ".mov";
        return ".mp4";
    }
    if (head.size() >= 4 && std::memcmp(head.data(), "OggS", 4) == 0) return ".ogg";
    if (head.size() >= 4 && std::memcmp(head.data(), "RIFF", 4) == 0) return ".wav";
    if (head.size() >= 3 && std::memcmp(head.data(), "ID3", 3) == 0) return ".mp3";
    if (head.size() >= 4 &&
        head[0] == 0x1a && head[1] == 0x45 && head[2] == 0xdf && head[3] == 0xa3) {
        return ".mkv";
    }
    return "";
}

} // namespace

// Default custom-I/O open: spill the entire stream to a temp file (sequential
// reads only, so a NULL seek callback is fine) and open that file. Backends
// that read callbacks natively override this.
avb_result AvbBackend::open_io(const avb_io_callbacks &cb, void *user,
                               const avb_decode_options &options) {
    if (!cb.read) return AVB_ERROR_INVALID_ARGUMENT;

    // Create a unique temp file path.
    std::string path;
#if defined(_WIN32)
    char dir[MAX_PATH];
    char file[MAX_PATH];
    if (!GetTempPathA(sizeof(dir), dir)) return AVB_ERROR_OPEN_FAILED;
    if (!GetTempFileNameA(dir, "avb", 0, file)) return AVB_ERROR_OPEN_FAILED;
    path = file;
    FILE *f = std::fopen(path.c_str(), "wb");
#else
    const char *tmpdir = std::getenv("TMPDIR");
    std::string tmpl = std::string(tmpdir && *tmpdir ? tmpdir : "/tmp") + "/avbridgeXXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = mkstemp(buf.data());
    if (fd < 0) return AVB_ERROR_OPEN_FAILED;
    path = buf.data();
    FILE *f = fdopen(fd, "wb");
#endif
    if (!f) { std::remove(path.c_str()); return AVB_ERROR_OPEN_FAILED; }

    unsigned char chunk[64 * 1024];
    std::vector<unsigned char> head;
    for (;;) {
        int n = cb.read(user, chunk, (int)sizeof(chunk));
        if (n < 0) { std::fclose(f); std::remove(path.c_str()); return AVB_ERROR_OPEN_FAILED; }
        if (n == 0) break;
        if (head.size() < 16) {
            size_t want = 16 - head.size();
            if ((size_t)n < want) want = (size_t)n;
            head.insert(head.end(), chunk, chunk + want);
        }
        if (std::fwrite(chunk, 1, (size_t)n, f) != (size_t)n) {
            std::fclose(f); std::remove(path.c_str()); return AVB_ERROR_OPEN_FAILED;
        }
    }
    std::fclose(f);

    const char *ext = avb_extension_for_magic(head);
    if (ext[0] != '\0') {
        std::string ext_path = path + ext;
        if (std::rename(path.c_str(), ext_path.c_str()) == 0) {
            path = ext_path;
        }
    }

    m_spill_path = path; // removed by ~AvbBackend
    return open_file(path.c_str(), options);
}

AvbBackend *avb_create_backend(avb_backend backend) {
    avb_backend resolved = backend;

    if (resolved == AVB_BACKEND_AUTO) {
#if defined(_WIN32)
        resolved = AVB_BACKEND_MEDIAFOUNDATION;
#elif defined(__APPLE__)
        resolved = AVB_BACKEND_AVFOUNDATION;
#elif defined(__linux__)
        // Linux default is GStreamer. If a build excludes the GStreamer backend
        // but includes FFmpeg, fall back to FFmpeg so AUTO still resolves.
#  if defined(AVB_ENABLE_GSTREAMER)
        resolved = AVB_BACKEND_GSTREAMER;
#  elif defined(AVB_ENABLE_FFMPEG)
        resolved = AVB_BACKEND_FFMPEG;
#  else
        return nullptr;
#  endif
#else
        return nullptr;
#endif
    }

    switch (resolved) {
#if defined(AVB_ENABLE_MEDIAFOUNDATION)
        case AVB_BACKEND_MEDIAFOUNDATION:
            return new AvbBackendMediaFoundation();
#endif
#if defined(AVB_ENABLE_AVFOUNDATION)
        case AVB_BACKEND_AVFOUNDATION:
            return new AvbBackendAVFoundation();
#endif
#if defined(AVB_ENABLE_FFMPEG)
        case AVB_BACKEND_FFMPEG:
            return new AvbBackendFFmpeg();
#endif
#if defined(AVB_ENABLE_GSTREAMER)
        case AVB_BACKEND_GSTREAMER:
            return new AvbBackendGStreamer();
#endif
        default:
            return nullptr;
    }
}
