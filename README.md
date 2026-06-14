# libavbridge

A small C/C++ library for decoding and encoding media through a portable C ABI.
It uses platform media frameworks or optional runtime-loaded FFmpeg/GStreamer
backends without bundling codec libraries.

## Backends

`AVB_BACKEND_AUTO` selects the platform default:

| Platform | Default |
| --- | --- |
| Windows | Media Foundation |
| macOS / iOS | AVFoundation |
| Linux | GStreamer |

FFmpeg is an optional cross-platform backend selected explicitly with
`AVB_BACKEND_FFMPEG`.

GStreamer and FFmpeg are loaded dynamically at runtime. Applications must
install the corresponding libraries and codec plugins separately.

## Features

- Audio and video decode, encode, probe, seek, and transcode APIs
- Static and runtime capability queries
- CPU, backend-native, and external hardware frame representations
- Hardware acceleration policies and device selection
- Custom video decoder and encoder plugins
- File, memory, and callback-based decode input

Codec and container support depends on the selected backend and installed
runtime. Use these APIs instead of assuming a fixed codec list:

- `avb_decoder_query_capabilities`
- `avb_encoder_query_capabilities`
- `avb_decoder_probe_runtime_capabilities`
- `avb_encoder_probe_runtime_capabilities`

## Video Memory

`avb_video_frame::memory_type` describes how a frame is exposed:

- `AVB_VIDEO_MEMORY_CPU`: CPU-readable planes
- `AVB_VIDEO_MEMORY_BACKEND_NATIVE`: an opaque backend object such as
  `AVFrame*` or `GstBuffer*`
- `AVB_VIDEO_MEMORY_EXTERNAL`: a typed platform interop representation

External representations use `external_type`:

| Type | Representation |
| --- | --- |
| `AVB_VIDEO_EXTERNAL_DMABUF` | DRM PRIME / DMABUF descriptors |
| `AVB_VIDEO_EXTERNAL_D3D11_TEXTURE` | `ID3D11Texture2D*` |
| `AVB_VIDEO_EXTERNAL_CVPIXEL_BUFFER` | `CVPixelBufferRef` |

Decode requests use `video_memory` and `video_external_type`. Encoder input
uses `input_memory` and `input_external_type`. Capability queries expose the
supported memory and external types separately.

Hardware behavior is controlled with:

- `AVB_HARDWARE_DISABLED`
- `AVB_HARDWARE_PREFER`
- `AVB_HARDWARE_REQUIRE`

The caller must release decoded frames with
`avb_decoder_release_video_frame`. For encoder input, the caller retains
ownership until `avb_encoder_write_video` returns.

## Custom Video Codecs

Applications can register process-wide codecs with:

- `avb_register_video_decoder`
- `avb_register_video_encoder`

Backends continue to handle demuxing, muxing, and audio while registered plugins
process matching video streams. This supports codecs such as HAP and GPU-ready
compressed formats including BC1, BC3, BC4, BC5, and BC7.

## Building

```bash
cmake -S . -B build
cmake --build build -j
```

Common options:

| Option | Default | Description |
| --- | --- | --- |
| `AVB_BUILD_SHARED` | ON | Build a shared library |
| `AVB_BUILD_EXAMPLES` | ON | Build example tools |
| `AVB_BUILD_TESTS` | OFF | Build tests |
| `AVB_ENABLE_MEDIAFOUNDATION` | Windows | Enable Media Foundation |
| `AVB_ENABLE_AVFOUNDATION` | Apple | Enable AVFoundation |
| `AVB_ENABLE_GSTREAMER` | Linux | Enable GStreamer |
| `AVB_ENABLE_FFMPEG` | OFF | Enable FFmpeg |

Build with FFmpeg and tests:

```bash
cmake -S . -B build \
  -DAVB_ENABLE_FFMPEG=ON \
  -DAVB_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Tests generate fixtures with the `ffmpeg` command-line tool when available.
Optional backend, codec, and hardware tests skip when unavailable.

## Runtime Dependencies

GStreamer requires the core, app, pbutils, video, and allocators libraries plus
the plugins needed by the selected codecs. DMABUF/VAAPI paths also require a
working VA driver and GStreamer VA plugins.

FFmpeg requires `libavformat`, `libavcodec`, `libavutil`, `libswresample`, and
`libswscale`. DMABUF/VAAPI paths require FFmpeg hardware-context support and a
working VA driver.

## Examples

```bash
build/examples/avb_probe sample.mp4
build/examples/avb_capabilities --backend ffmpeg --runtime
build/examples/avb_decode_audio sample.mp4 out.f32
build/examples/avb_decode_video sample.mp4 frame_%04d.rgba
build/examples/avb_transcode sample.mp4 out.mp4
```

Select codecs and hardware explicitly:

```bash
build/examples/avb_transcode sample.mp4 out.webm \
  --backend gstreamer \
  --video-codec vp9 \
  --audio-codec opus \
  --hardware require \
  --hardware-device vaapi
```

## License

libavbridge is licensed under the MIT License. See [LICENSE](LICENSE).

GStreamer and FFmpeg are external runtime dependencies. They are not bundled or
linked into libavbridge.
