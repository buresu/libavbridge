# libavbridge

A small, portable C/C++ library that provides a unified API for decoding media
files across platforms. It abstracts the platform-native or platform-preferred
media stack behind a stable C ABI, so applications and plugins can add
audio/video decoding without embedding large codec libraries directly.

## Backends

libavbridge uses platform media backends. `AVB_BACKEND_AUTO` selects the default
per platform:

| Platform | Default backend (`auto`) |
| -------- | ------------------------ |
| Windows  | `mediafoundation`        |
| macOS / iOS | `avfoundation`        |
| Linux    | `gstreamer`              |

`ffmpeg` is an **optional, cross-platform** backend (Windows / macOS / Linux).
Enable it at build time and select it explicitly with `AVB_BACKEND_FFMPEG`; it
is never chosen by `auto`.

The `gstreamer` and `ffmpeg` backends are **loaded at runtime** via dynamic
loading (`dlopen`/`dlsym`, or `LoadLibrary`/`GetProcAddress` on Windows). This
project does **not** bundle GStreamer or FFmpeg and does **not** link to them at
build time — the relevant runtime libraries must be installed on the target
system. If the selected backend's libraries are missing, decoding is
unavailable and opening fails with a clear backend-unavailable error.

### Runtime libraries

GStreamer backend (Linux default):

- `libgstreamer-1.0`, `libgstapp-1.0`, `libgstpbutils-1.0`
- `libglib-2.0`, `libgobject-2.0`
- GStreamer plugins: `base`, `good` (plus others for additional codecs)

FFmpeg backend (optional):

- `libavformat`, `libavcodec`, `libavutil`, `libswresample`, `libswscale`

> Users and distributors are responsible for ensuring that their installed
> GStreamer / FFmpeg builds and codec usage comply with applicable licenses and
> patent requirements. FDK-AAC is not used.

## Building

```bash
cmake -S . -B build
cmake --build build -j
```

### CMake options

| Option                       | Default                     | Description                                   |
| ---------------------------- | --------------------------- | --------------------------------------------- |
| `AVB_BUILD_SHARED`           | ON                          | Build the shared library                      |
| `AVB_BUILD_EXAMPLES`         | ON                          | Build the example tools                       |
| `AVB_BUILD_TESTS`            | OFF                         | Build the conformance test                    |
| `AVB_ENABLE_MEDIAFOUNDATION` | ON (Windows only)           | Windows Media Foundation backend              |
| `AVB_ENABLE_AVFOUNDATION`    | ON (Apple only)             | Apple AVFoundation backend                    |
| `AVB_ENABLE_GSTREAMER`       | ON on Linux, OFF elsewhere  | GStreamer backend (Linux default)             |
| `AVB_ENABLE_FFMPEG`          | OFF                         | FFmpeg backend (optional, cross-platform)     |

Examples:

```bash
# Linux default build (GStreamer backend)
cmake -S . -B build && cmake --build build -j

# Add the optional FFmpeg backend alongside GStreamer
cmake -S . -B build -DAVB_ENABLE_FFMPEG=ON && cmake --build build -j

# Run the conformance test (needs the `ffmpeg` CLI to generate the fixture)
cmake -S . -B build -DAVB_BUILD_TESTS=ON && cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Examples

```bash
build/examples/avb_probe         sample.mp4
build/examples/avb_decode_audio  sample.mp4 out.f32
build/examples/avb_decode_video  sample.mp4 frame_%04d.rgba
```

## License

libavbridge is licensed under the MIT License. See [LICENSE](LICENSE) for
details.

GStreamer and FFmpeg are external runtime dependencies loaded dynamically; they
are neither bundled nor linked at build time.
