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

## Codec support

The public encoder API can request H.264, HEVC, VP8, VP9, AV1, HAP, AAC, and
Opus. Exact availability is backend, container, and runtime-install dependent:

- FFmpeg can encode H.264, HEVC, VP8, VP9, AV1, HAP, AAC, and Opus when the
  installed FFmpeg build provides the corresponding encoder.
- GStreamer can encode H.264, HEVC, VP8, VP9, AV1, HAP, AAC, and Opus when the
  required plugins are installed.
- AVFoundation and Media Foundation built-in encoders are limited to H.264 and
  HEVC. VP8, VP9, AV1, and HAP can still be produced through registered custom
  video encoders when the selected container accepts the compressed packets.

## Hardware video frames

`avb_video_frame` can describe either CPU-readable planes or backend-native
hardware frames. Use `avb_decode_options::video_memory` and
`hardware_policy`/`hardware_device` to request native decode output, and use
`avb_video_encode_params::input_memory` plus the same hardware options to feed
native frames into encoders.

Memory modes:

- `AVB_VIDEO_MEMORY_CPU` is the portable default. Frames use `plane_data[]`,
  `plane_stride[]`, `data`, and `stride`.
- `AVB_VIDEO_MEMORY_NATIVE` keeps the backend's native object alive until the
  frame is released. FFmpeg uses `AVFrame*`; GStreamer uses `GstBuffer*`;
  platform backends should use their reference-counted surface object.
- `AVB_VIDEO_MEMORY_DMABUF` is the Linux zero-copy interchange mode. Frames
  carry DRM PRIME / DMABUF fd, offset, stride, modifier, and `drm_format`
  metadata. Multiple planes may point at the same fd.

Hardware policy:

- `AVB_HARDWARE_DISABLED` keeps the request on CPU/system-memory paths.
- `AVB_HARDWARE_PREFER` enables hardware when the backend can do so and keeps a
  CPU fallback only where such a fallback preserves the requested memory type.
- `AVB_HARDWARE_REQUIRE` makes `open` fail unless the backend can satisfy the
  requested codec, device, and memory type in hardware.

Implemented native paths:

- FFmpeg decode returns backend-owned `AVFrame*` handles for hardware frames;
  VAAPI also exposes the `VASurfaceID` through `native_handle_id`.
- FFmpeg encode supports VAAPI hardware encoding, including CPU-frame upload and
  direct `AVFrame*` native input when it matches the encoder device.
- GStreamer decode can request `video/x-raw(memory:VASurface)` and returns a
  `GstBuffer*` native handle.
- GStreamer encode can push native `GstBuffer*` input into VA encoders, or upload
  CPU input through `vapostproc`.
- FFmpeg decode can export hardware frames as DRM PRIME / DMABUF descriptors,
  filling `dmabuf_fd[]`, `plane_offset[]`, `plane_stride[]`,
  `dmabuf_modifier[]`, and `drm_format`.
- GStreamer decode can request `video/x-raw(memory:DMABuf)` and fill the same
  DMABUF fields from the returned buffer.
- GStreamer encode can import DMABUF input either by reusing a `GstBuffer*`
  native handle or by wrapping the `dmabuf_fd[]` planes into a new buffer.
- FFmpeg encode can import DMABUF input into the VAAPI hardware encoder path by
  wrapping the plane descriptors as a DRM PRIME `AVFrame`.

The optional `avb_dmabuf_roundtrip` CTest smoke validates all Linux VAAPI
interchange paths when the runtime stack supports them:

- FFmpeg decode -> FFmpeg encode
- FFmpeg decode -> GStreamer encode
- GStreamer decode -> GStreamer encode
- GStreamer decode -> FFmpeg encode

On systems without VAAPI, DMABUF support, or the relevant plugins/codecs, those
tests skip rather than fail.

### Runtime libraries

GStreamer backend (Linux default):

- `libgstreamer-1.0`, `libgstapp-1.0`, `libgstpbutils-1.0`
- `libgstvideo-1.0`, `libgstallocators-1.0`
- `libglib-2.0`, `libgobject-2.0`
- GStreamer plugins: `base`, `good` (plus others for additional codecs)
- For VAAPI native/DMABUF paths: the GStreamer `va` plugin (for example
  `gst-plugin-va` on Arch Linux), a working VA driver, and `vainfo` reporting
  the required decode/encode entrypoints.

FFmpeg backend (optional):

- `libavformat`, `libavcodec`, `libavutil`, `libswresample`, `libswscale`
- For VAAPI native/DMABUF paths: an FFmpeg build with VAAPI and DRM PRIME
  hwcontext support, plus a working VA driver.

> Users and distributors are responsible for ensuring that their installed
> GStreamer / FFmpeg builds and codec usage comply with applicable licenses and
> patent requirements. FDK-AAC is not used.

## Custom video decoders

Applications can register process-wide custom video decoders with
`avb_register_video_decoder`. Capable backends still handle demuxing and regular
audio decoding, then route matching video packets to the registered decoder.
This is intended for formats such as HAP where a plugin may want to return
GPU-ready compressed frames instead of CPU-expanded pixels. FFmpeg, GStreamer,
Media Foundation, and AVFoundation can use custom video decoders.

Applications can also register custom video encoders with
`avb_register_video_encoder`. FFmpeg, GStreamer, Media Foundation, and
AVFoundation can use a registered encoder for video compression while continuing
to mux regular audio through the backend. FFmpeg writes the returned encoded
packets directly to the container muxer; GStreamer pushes them through an
encoded `appsrc` using the caps reported by the plugin; Media Foundation writes
encoded samples directly to Sink Writer streams when the selected Windows media
sink accepts that compressed format; AVFoundation wraps them in
`CMSampleBuffer` objects for `AVAssetWriter`.

Compressed block formats are represented through `avb_video_frame` using
`AVB_PIXEL_FORMAT_BC1_RGBA`, `AVB_PIXEL_FORMAT_BC3_RGBA`, `AVB_PIXEL_FORMAT_BC4_R`,
`AVB_PIXEL_FORMAT_BC5_RG`, or `AVB_PIXEL_FORMAT_BC7_RGBA`. For these formats,
`data` points to the compressed payload and `stride` is the byte count for one
row of 4x4 blocks.

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
build/examples/avb_probe         sample.mp4 ffmpeg
build/examples/avb_decode_audio  sample.mp4 out.f32
build/examples/avb_decode_video  sample.mp4 frame_%04d.rgba
build/examples/avb_transcode     sample.mp4 out.mp4

# Select backend/codecs explicitly.
build/examples/avb_transcode sample.mp4 out.webm \
  --backend gstreamer --video-codec vp9 --audio-codec opus
build/examples/avb_transcode sample.mp4 out.mkv \
  --backend ffmpeg --video-codec av1 --audio-codec opus

# Require a specific hardware encoder path when the runtime stack supports it.
build/examples/avb_transcode sample.mp4 out.webm \
  --backend gstreamer --video-codec vp9 --audio-codec opus \
  --hardware require --hardware-device vaapi
```

## License

libavbridge is licensed under the MIT License. See [LICENSE](LICENSE) for
details.

GStreamer and FFmpeg are external runtime dependencies loaded dynamically; they
are neither bundled nor linked at build time.
