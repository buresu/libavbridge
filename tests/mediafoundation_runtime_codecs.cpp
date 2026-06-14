// Media Foundation runtime capability smoke.
//
// If the Windows runtime reports optional codec support, verify that the
// backend can actually exercise a representative session. Systems without the
// optional MFTs skip cleanly.

#include <avbridge.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <d3d11.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#endif

static const int AVB_TEST_SKIP = 77;

static bool has_video_codec(const avb_encoder_capabilities &caps,
                            avb_video_codec codec) {
    for (int i = 0; i < caps.video_codec_count; ++i) {
        if (caps.video_codecs[i] == codec) return true;
    }
    return false;
}

static bool has_audio_codec(const avb_encoder_capabilities &caps,
                            avb_audio_codec codec) {
    for (int i = 0; i < caps.audio_codec_count; ++i) {
        if (caps.audio_codecs[i] == codec) return true;
    }
    return false;
}

static bool has_video_codec(const avb_decoder_capabilities &caps,
                            avb_video_codec codec) {
    for (int i = 0; i < caps.video_codec_count; ++i) {
        if (caps.video_codecs[i] == codec) return true;
    }
    return false;
}

static bool has_audio_codec(const avb_decoder_capabilities &caps,
                            avb_audio_codec codec) {
    for (int i = 0; i < caps.audio_codec_count; ++i) {
        if (caps.audio_codecs[i] == codec) return true;
    }
    return false;
}

static std::string probe_video_codec(const char *path) {
    avb_decode_options opts = avb_decode_options_default();
    opts.backend = AVB_BACKEND_MEDIAFOUNDATION;
    opts.enable_audio = 0;
    opts.enable_video = 1;

    avb_decoder *dec = nullptr;
    std::string codec;
    if (avb_decoder_open(&dec, path, &opts) == AVB_OK) {
        avb_media_info info{};
        if (avb_decoder_get_media_info(dec, &info) == AVB_OK &&
            info.video.codec_name) {
            codec = info.video.codec_name;
        }
    }
    avb_decoder_close(dec);
    return codec;
}

static int smoke_audio_decoder(const char *path, avb_audio_codec codec,
                               const char *expected_name) {
    avb_decoder_capabilities caps{};
    if (avb_decoder_probe_runtime_capabilities(
            AVB_BACKEND_MEDIAFOUNDATION, path, &caps) != AVB_OK ||
        caps.result == AVB_ERROR_BACKEND_NOT_AVAILABLE) {
        std::printf("SKIP: Media Foundation runtime is not available\n");
        return AVB_TEST_SKIP;
    }
    if (caps.result != AVB_OK || !has_audio_codec(caps, codec)) {
        std::printf("SKIP: decoder MFT is not available for %s\n",
                    avb_audio_codec_name(codec));
        return AVB_TEST_SKIP;
    }

    avb_decode_options opts = avb_decode_options_default();
    opts.backend = AVB_BACKEND_MEDIAFOUNDATION;
    opts.enable_video = 0;
    opts.enable_audio = 1;

    avb_decoder *dec = nullptr;
    if (avb_decoder_open(&dec, path, &opts) != AVB_OK) {
        std::fprintf(stderr, "open %s failed: %s\n", path,
                     avb_decoder_get_last_error(dec)
                        ? avb_decoder_get_last_error(dec) : "unknown");
        avb_decoder_close(dec);
        return 1;
    }

    avb_media_info info{};
    avb_decoder_get_media_info(dec, &info);
    const char *actual = info.audio.codec_name ? info.audio.codec_name : "";
    if (expected_name && expected_name[0] &&
        std::strcmp(expected_name, "*") != 0 &&
        std::strcmp(actual, expected_name) != 0) {
        std::fprintf(stderr, "expected %s audio codec '%s', got '%s'\n",
                     path, expected_name, actual);
        avb_decoder_close(dec);
        return 1;
    }

    int channels = info.audio.channels > 0 ? info.audio.channels : 1;
    std::vector<float> pcm(4096 * channels);
    int got = avb_decoder_read_audio_f32(dec, pcm.data(), 4096, nullptr);
    if (got <= 0) {
        std::fprintf(stderr, "read %s produced no audio frames\n", path);
        avb_decoder_close(dec);
        return 1;
    }

    std::printf("Media Foundation %s decode smoke passed (%d frames, codec '%s')\n",
                avb_audio_codec_name(codec), got, actual);
    avb_decoder_close(dec);
    return 0;
}

static int smoke_video_decoder(const char *path, avb_video_codec codec,
                               const char *expected_name) {
    avb_decoder_capabilities caps{};
    if (avb_decoder_probe_runtime_capabilities(
            AVB_BACKEND_MEDIAFOUNDATION, path, &caps) != AVB_OK ||
        caps.result == AVB_ERROR_BACKEND_NOT_AVAILABLE) {
        std::printf("SKIP: Media Foundation runtime is not available\n");
        return AVB_TEST_SKIP;
    }
    if (caps.result != AVB_OK || !has_video_codec(caps, codec)) {
        std::printf("SKIP: decoder MFT is not available for %s\n",
                    avb_video_codec_name(codec));
        return AVB_TEST_SKIP;
    }

    avb_decode_options opts = avb_decode_options_default();
    opts.backend = AVB_BACKEND_MEDIAFOUNDATION;
    opts.enable_audio = 0;
    opts.enable_video = 1;
    opts.video_format = AVB_PIXEL_FORMAT_BGRA8;

    avb_decoder *dec = nullptr;
    if (avb_decoder_open(&dec, path, &opts) != AVB_OK) {
        std::fprintf(stderr, "open %s failed: %s\n", path,
                     avb_decoder_get_last_error(dec)
                        ? avb_decoder_get_last_error(dec) : "unknown");
        avb_decoder_close(dec);
        return 1;
    }

    avb_media_info info{};
    avb_decoder_get_media_info(dec, &info);
    const char *actual = info.video.codec_name ? info.video.codec_name : "";
    if (expected_name && expected_name[0] &&
        std::strcmp(expected_name, "*") != 0 &&
        std::strcmp(actual, expected_name) != 0) {
        std::fprintf(stderr, "expected %s video codec '%s', got '%s'\n",
                     path, expected_name, actual);
        avb_decoder_close(dec);
        return 1;
    }

    avb_video_frame frame{};
    avb_result read = avb_decoder_read_video_frame(dec, &frame);
    if (read != AVB_OK) {
        std::fprintf(stderr, "read %s produced no video frame (%d): %s\n",
                     path, (int)read,
                     avb_decoder_get_last_error(dec)
                        ? avb_decoder_get_last_error(dec) : "unknown");
        avb_decoder_close(dec);
        return 1;
    }
    bool frame_ok = frame.width > 0 && frame.height > 0 &&
                    frame.data != nullptr && frame.stride > 0;
    avb_decoder_release_video_frame(dec, &frame);
    if (!frame_ok) {
        std::fprintf(stderr, "read %s produced an invalid video frame\n", path);
        avb_decoder_close(dec);
        return 1;
    }

    std::printf("Media Foundation %s decode smoke passed (%dx%d, codec '%s')\n",
                avb_video_codec_name(codec), info.video.width,
                info.video.height, actual);
    avb_decoder_close(dec);
    return 0;
}

static int smoke_ivf_output_formats(const char *path) {
    const avb_pixel_format formats[] = {
        AVB_PIXEL_FORMAT_NV12,
        AVB_PIXEL_FORMAT_I420,
        AVB_PIXEL_FORMAT_RGBA8,
    };
    for (avb_pixel_format format : formats) {
        avb_decode_options opts = avb_decode_options_default();
        opts.backend = AVB_BACKEND_MEDIAFOUNDATION;
        opts.enable_audio = 0;
        opts.enable_video = 1;
        opts.video_format = format;

        avb_decoder *dec = nullptr;
        if (avb_decoder_open(&dec, path, &opts) != AVB_OK) {
            std::fprintf(stderr, "open IVF output format %d failed: %s\n",
                         (int)format,
                         avb_decoder_get_last_error(dec)
                            ? avb_decoder_get_last_error(dec) : "unknown");
            avb_decoder_close(dec);
            return 1;
        }
        avb_video_frame frame{};
        if (avb_decoder_read_video_frame(dec, &frame) != AVB_OK ||
            frame.format != format || !frame.data ||
            frame.width <= 0 || frame.height <= 0) {
            std::fprintf(stderr, "IVF output format %d produced an invalid frame\n",
                         (int)format);
            avb_decoder_close(dec);
            return 1;
        }
        avb_decoder_release_video_frame(dec, &frame);
        double landed = -1.0;
        if (avb_decoder_seek(dec, 0.0, &landed) != AVB_OK ||
            avb_decoder_read_video_frame(dec, &frame) != AVB_OK) {
            std::fprintf(stderr, "IVF seek/reset failed for output format %d\n",
                         (int)format);
            avb_decoder_close(dec);
            return 1;
        }
        avb_decoder_release_video_frame(dec, &frame);
        avb_decoder_close(dec);
    }
    return 0;
}

static int smoke_audio_encoder(const char *input_path, const char *output_path,
                               avb_audio_codec codec,
                               const char *expected_name,
                               const char *label) {
    avb_encoder_capabilities caps{};
    if (avb_encoder_probe_runtime_capabilities(
            AVB_BACKEND_MEDIAFOUNDATION, output_path, &caps) != AVB_OK ||
        caps.result == AVB_ERROR_BACKEND_NOT_AVAILABLE) {
        std::printf("SKIP: Media Foundation runtime is not available\n");
        return AVB_TEST_SKIP;
    }
    if (caps.result != AVB_OK ||
        !has_audio_codec(caps, codec)) {
        std::printf("SKIP: %s encoder sink is not available\n", label);
        return AVB_TEST_SKIP;
    }

    avb_decode_options dopts = avb_decode_options_default();
    dopts.backend = AVB_BACKEND_MEDIAFOUNDATION;
    dopts.enable_video = 0;
    dopts.enable_audio = 1;
    avb_decoder *dec = nullptr;
    if (avb_decoder_open(&dec, input_path, &dopts) != AVB_OK) {
        std::fprintf(stderr, "open audio input failed: %s\n",
                     avb_decoder_get_last_error(dec)
                        ? avb_decoder_get_last_error(dec) : "unknown");
        avb_decoder_close(dec);
        return 1;
    }

    avb_media_info info{};
    avb_decoder_get_media_info(dec, &info);
    int channels = info.audio.channels > 0 ? info.audio.channels : 1;
    int sample_rate = info.audio.sample_rate > 0 ? info.audio.sample_rate : 44100;

    avb_encode_options eopts = avb_encode_options_default();
    eopts.backend = AVB_BACKEND_MEDIAFOUNDATION;
    eopts.audio.enable = 1;
    eopts.audio.codec = codec;
    eopts.audio.channels = channels;
    eopts.audio.sample_rate = sample_rate;

    avb_encoder *enc = nullptr;
    if (avb_encoder_open(&enc, output_path, &eopts) != AVB_OK) {
        std::fprintf(stderr, "open %s output failed: %s\n", label,
                     avb_encoder_get_last_error(enc)
                        ? avb_encoder_get_last_error(enc) : "unknown");
        avb_encoder_close(enc);
        avb_decoder_close(dec);
        return 1;
    }

    std::vector<float> pcm(4096 * channels);
    int total = 0;
    while (total < sample_rate) {
        int got = avb_decoder_read_audio_f32(dec, pcm.data(), 4096, nullptr);
        if (got <= 0) break;
        if (avb_encoder_write_audio_f32(enc, pcm.data(), got) != AVB_OK) {
            std::fprintf(stderr, "write %s failed: %s\n", label,
                         avb_encoder_get_last_error(enc)
                            ? avb_encoder_get_last_error(enc) : "unknown");
            avb_encoder_close(enc);
            avb_decoder_close(dec);
            return 1;
        }
        total += got;
    }

    avb_result finish = avb_encoder_finish(enc);
    if (finish != AVB_OK) {
        std::fprintf(stderr, "finish %s encode failed: %s\n", label,
                     avb_encoder_get_last_error(enc)
                        ? avb_encoder_get_last_error(enc) : "unknown");
        avb_encoder_close(enc);
        avb_decoder_close(dec);
        return 1;
    }
    avb_encoder_close(enc);
    avb_decoder_close(dec);

    int decode_res = smoke_audio_decoder(output_path, codec, expected_name);
    if (decode_res == 1) return 1;
    if (decode_res == AVB_TEST_SKIP) return decode_res;

    std::printf("Media Foundation %s encode smoke passed (%d frames)\n",
                label, total);
    return 0;
}

static uint32_t read_le32(const unsigned char *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int smoke_ivf_video_encoder(const char *input_path,
                                   const char *output_path,
                                   avb_video_codec codec,
                                   const char *fourcc,
                                   const char *label) {
    avb_encoder_capabilities caps{};
    if (avb_encoder_probe_runtime_capabilities(
            AVB_BACKEND_MEDIAFOUNDATION, output_path, &caps) != AVB_OK ||
        caps.result == AVB_ERROR_BACKEND_NOT_AVAILABLE) {
        std::printf("SKIP: Media Foundation runtime is not available\n");
        return AVB_TEST_SKIP;
    }
    if (caps.result != AVB_OK || !has_video_codec(caps, codec)) {
        std::printf("SKIP: %s IVF encoder MFT is not configurable\n", label);
        return AVB_TEST_SKIP;
    }

    avb_decode_options dopts = avb_decode_options_default();
    dopts.backend = AVB_BACKEND_MEDIAFOUNDATION;
    dopts.enable_audio = 0;
    dopts.enable_video = 1;
    dopts.video_format = AVB_PIXEL_FORMAT_I420;

    avb_decoder *dec = nullptr;
    if (avb_decoder_open(&dec, input_path, &dopts) != AVB_OK) {
        std::fprintf(stderr, "open %s failed: %s\n", input_path,
                     avb_decoder_get_last_error(dec)
                        ? avb_decoder_get_last_error(dec) : "unknown");
        avb_decoder_close(dec);
        return 1;
    }
    avb_media_info info{};
    avb_decoder_get_media_info(dec, &info);

    avb_encode_options eopts = avb_encode_options_default();
    eopts.backend = AVB_BACKEND_MEDIAFOUNDATION;
    eopts.video.enable = 1;
    eopts.video.width = info.video.width;
    eopts.video.height = info.video.height;
    eopts.video.frame_rate = info.video.frame_rate > 0.0 ? info.video.frame_rate : 30.0;
    eopts.video.codec = codec;
    eopts.video.input_format = AVB_PIXEL_FORMAT_I420;
    eopts.video.bitrate = 1000000;

    avb_encoder *enc = nullptr;
    if (avb_encoder_open(&enc, output_path, &eopts) != AVB_OK) {
        std::printf("SKIP: %s IVF encoder did not accept the test session: %s\n",
                    label,
                    avb_encoder_get_last_error(enc)
                        ? avb_encoder_get_last_error(enc) : "unknown");
        avb_encoder_close(enc);
        avb_decoder_close(dec);
        return AVB_TEST_SKIP;
    }

    int frames = 0;
    avb_video_frame frame{};
    while (frames < 30 && avb_decoder_read_video_frame(dec, &frame) == AVB_OK) {
        avb_result wr = avb_encoder_write_video(enc, &frame, frame.pts_sec);
        avb_decoder_release_video_frame(dec, &frame);
        if (wr != AVB_OK) {
            std::fprintf(stderr, "write %s IVF frame failed: %s\n", label,
                         avb_encoder_get_last_error(enc)
                            ? avb_encoder_get_last_error(enc) : "unknown");
            avb_encoder_close(enc);
            avb_decoder_close(dec);
            return 1;
        }
        ++frames;
    }
    if (avb_encoder_finish(enc) != AVB_OK) {
        std::fprintf(stderr, "finish %s IVF encode failed: %s\n", label,
                     avb_encoder_get_last_error(enc)
                        ? avb_encoder_get_last_error(enc) : "unknown");
        avb_encoder_close(enc);
        avb_decoder_close(dec);
        return 1;
    }
    avb_encoder_close(enc);
    avb_decoder_close(dec);

    FILE *f = std::fopen(output_path, "rb");
    unsigned char header[32] = {};
    if (!f || std::fread(header, 1, sizeof(header), f) != sizeof(header)) {
        if (f) std::fclose(f);
        std::fprintf(stderr, "read %s IVF header failed\n", label);
        return 1;
    }
    std::fclose(f);
    if (std::memcmp(header, "DKIF", 4) != 0 ||
        std::memcmp(header + 8, fourcc, 4) != 0 ||
        read_le32(header + 24) == 0) {
        std::fprintf(stderr, "%s IVF header is invalid\n", label);
        return 1;
    }

    int decode_res = smoke_video_decoder(
        output_path, codec, avb_video_codec_name(codec));
    if (decode_res != 0) {
        if (decode_res == AVB_TEST_SKIP)
            std::fprintf(stderr, "%s IVF output was not decodable\n", label);
        return 1;
    }
    if (smoke_ivf_output_formats(output_path) != 0) return 1;

    std::printf("Media Foundation %s IVF encode smoke passed (%u frames)\n",
                label, read_le32(header + 24));
    return 0;
}

#ifdef _WIN32
static int smoke_native_video_encoder(const char *output_path,
                                      avb_video_codec codec,
                                      const char *label) {
    avb_encoder_capabilities caps{};
    if (avb_encoder_probe_runtime_capabilities(
            AVB_BACKEND_MEDIAFOUNDATION, output_path, &caps) != AVB_OK ||
        caps.result != AVB_OK ||
        !has_video_codec(caps, codec)) {
        std::printf("SKIP: native %s encoder MFT is not available\n", label);
        return AVB_TEST_SKIP;
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL feature_level{};
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
        D3D11_SDK_VERSION, &device, &feature_level, &context);
    if (FAILED(hr)) {
        std::printf("SKIP: D3D11 device creation failed: 0x%08lx\n", hr);
        return AVB_TEST_SKIP;
    }

    const int width = 320;
    const int height = 240;
    std::vector<unsigned char> nv12((size_t)width * height * 3 / 2, 128);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x)
            nv12[(size_t)y * width + x] =
                (unsigned char)(16 + ((x + y) % 220));
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = nv12.data();
    initial.SysMemPitch = width;
    initial.SysMemSlicePitch = (UINT)nv12.size();
    ComPtr<ID3D11Texture2D> texture;
    hr = device->CreateTexture2D(&desc, &initial, &texture);
    if (FAILED(hr)) {
        std::printf("SKIP: NV12 texture creation failed: 0x%08lx\n", hr);
        return AVB_TEST_SKIP;
    }

    avb_encode_options opts = avb_encode_options_default();
    opts.backend = AVB_BACKEND_MEDIAFOUNDATION;
    opts.video.enable = 1;
    opts.video.width = width;
    opts.video.height = height;
    opts.video.frame_rate = 15.0;
    opts.video.codec = codec;
    opts.video.input_format = AVB_PIXEL_FORMAT_NV12;
    opts.video.input_memory = AVB_VIDEO_MEMORY_EXTERNAL;
    opts.video.input_external_type = AVB_VIDEO_EXTERNAL_D3D11_TEXTURE;
    opts.video.hardware_policy = AVB_HARDWARE_REQUIRE;
    opts.video.hardware_device = AVB_HW_DEVICE_D3D11VA;
    opts.video.hardware_context =
        std::strstr(output_path, ".ivf") ? nullptr : device.Get();

    avb_encoder *enc = nullptr;
    if (avb_encoder_open(&enc, output_path, &opts) != AVB_OK) {
        std::fprintf(stderr, "open native %s output failed: %s\n", label,
                     avb_encoder_get_last_error(enc)
                        ? avb_encoder_get_last_error(enc) : "unknown");
        avb_encoder_close(enc);
        return 1;
    }

    avb_video_frame frame{};
    frame.width = width;
    frame.height = height;
    frame.format = AVB_PIXEL_FORMAT_NV12;
    frame.memory_type = AVB_VIDEO_MEMORY_EXTERNAL;
    frame.external_type = AVB_VIDEO_EXTERNAL_D3D11_TEXTURE;
    frame.hardware_device = AVB_HW_DEVICE_D3D11VA;
    frame.native_handle = texture.Get();
    for (int i = 0; i < 7; ++i) {
        frame.pts_sec = (double)i / 15.0;
        if (avb_encoder_write_video(enc, &frame, frame.pts_sec) != AVB_OK) {
            std::fprintf(stderr, "write native %s frame failed: %s\n", label,
                         avb_encoder_get_last_error(enc)
                            ? avb_encoder_get_last_error(enc) : "unknown");
            avb_encoder_close(enc);
            return 1;
        }
    }
    if (avb_encoder_finish(enc) != AVB_OK) {
        std::fprintf(stderr, "finish native %s failed: %s\n", label,
                     avb_encoder_get_last_error(enc)
                        ? avb_encoder_get_last_error(enc) : "unknown");
        avb_encoder_close(enc);
        return 1;
    }
    avb_encoder_close(enc);

    int decoded = smoke_video_decoder(
        output_path, codec, avb_video_codec_name(codec));
    if (decoded != 0) return decoded == AVB_TEST_SKIP ? 1 : decoded;
    std::printf("Media Foundation native D3D11 %s encode smoke passed\n",
                label);
    return 0;
}

static int smoke_native_ivf_decoder(const char *input_path,
                                    const char *label) {
    avb_decode_options dopts = avb_decode_options_default();
    dopts.backend = AVB_BACKEND_MEDIAFOUNDATION;
    dopts.enable_audio = 0;
    dopts.enable_video = 1;
    dopts.video_format = AVB_PIXEL_FORMAT_NV12;
    dopts.video_memory = AVB_VIDEO_MEMORY_EXTERNAL;
    dopts.video_external_type = AVB_VIDEO_EXTERNAL_D3D11_TEXTURE;
    dopts.hardware_policy = AVB_HARDWARE_REQUIRE;
    dopts.hardware_device = AVB_HW_DEVICE_D3D11VA;

    ComPtr<ID3D11Device> expected_device;
    D3D_FEATURE_LEVEL feature_level{};
    HRESULT device_hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
        D3D11_SDK_VERSION, &expected_device, &feature_level, nullptr);
    if (FAILED(device_hr)) {
        std::printf("SKIP: native %s IVF D3D11 device is not available\n",
                    label);
        return AVB_TEST_SKIP;
    }
    dopts.hardware_context = expected_device.Get();

    avb_decoder *dec = nullptr;
    if (avb_decoder_open(&dec, input_path, &dopts) != AVB_OK) {
        std::printf("SKIP: native %s IVF decoder MFT is not available: %s\n",
                    label, avb_decoder_get_last_error(dec)
                        ? avb_decoder_get_last_error(dec) : "unknown");
        avb_decoder_close(dec);
        return AVB_TEST_SKIP;
    }
    int frames = 0;
    avb_video_frame frame{};
    while (frames < 7 && avb_decoder_read_video_frame(dec, &frame) == AVB_OK) {
        if (frame.memory_type != AVB_VIDEO_MEMORY_EXTERNAL ||
            frame.external_type != AVB_VIDEO_EXTERNAL_D3D11_TEXTURE ||
            frame.hardware_device != AVB_HW_DEVICE_D3D11VA ||
            !frame.native_handle ||
            frame.format != AVB_PIXEL_FORMAT_NV12) {
            std::fprintf(stderr, "native IVF decoder returned a non-D3D11 frame\n");
            avb_decoder_release_video_frame(dec, &frame);
            avb_decoder_close(dec);
            return 1;
        }
        ComPtr<ID3D11Device> texture_device;
        static_cast<ID3D11Texture2D *>(frame.native_handle)
            ->GetDevice(&texture_device);
        if (texture_device.Get() != expected_device.Get()) {
            std::fprintf(stderr,
                         "native %s IVF decoder ignored the requested device\n",
                         label);
            avb_decoder_release_video_frame(dec, &frame);
            avb_decoder_close(dec);
            return 1;
        }
        avb_decoder_release_video_frame(dec, &frame);
        ++frames;
    }
    if (frames == 0) {
        std::fprintf(stderr, "native %s IVF decoder produced no frames\n", label);
        avb_decoder_close(dec);
        return 1;
    }
    avb_decoder_close(dec);

    std::printf(
        "Media Foundation native D3D11 %s IVF decode smoke passed (%d frames)\n",
        label, frames);
    return 0;
}

static int smoke_native_source_decoder(const char *input_path,
                                       const char *label,
                                       bool required = true,
                                       ID3D11Device *expected_device = nullptr) {
    avb_decode_options opts = avb_decode_options_default();
    opts.backend = AVB_BACKEND_MEDIAFOUNDATION;
    opts.enable_audio = 0;
    opts.enable_video = 1;
    opts.video_format = AVB_PIXEL_FORMAT_NV12;
    opts.video_memory = AVB_VIDEO_MEMORY_EXTERNAL;
    opts.video_external_type = AVB_VIDEO_EXTERNAL_D3D11_TEXTURE;
    opts.hardware_policy = AVB_HARDWARE_REQUIRE;
    opts.hardware_device = AVB_HW_DEVICE_D3D11VA;
    opts.hardware_context = expected_device;

    avb_decoder *dec = nullptr;
    if (avb_decoder_open(&dec, input_path, &opts) != AVB_OK) {
        std::fprintf(required ? stderr : stdout,
                     required
                        ? "open native %s decoder failed: %s\n"
                        : "SKIP: native %s decoder is not available: %s\n",
                     label, avb_decoder_get_last_error(dec)
                         ? avb_decoder_get_last_error(dec) : "unknown");
        avb_decoder_close(dec);
        return required ? 1 : AVB_TEST_SKIP;
    }

    for (int pass = 0; pass < 2; ++pass) {
        avb_video_frame frame{};
        avb_result read = avb_decoder_read_video_frame(dec, &frame);
        if (read != AVB_OK ||
            frame.memory_type != AVB_VIDEO_MEMORY_EXTERNAL ||
            frame.external_type != AVB_VIDEO_EXTERNAL_D3D11_TEXTURE ||
            frame.hardware_device != AVB_HW_DEVICE_D3D11VA ||
            frame.format != AVB_PIXEL_FORMAT_NV12 ||
            !frame.native_handle) {
            std::fprintf(stderr,
                         "native %s decoder did not return a D3D11 NV12 frame\n",
                         label);
            if (read == AVB_OK)
                avb_decoder_release_video_frame(dec, &frame);
            avb_decoder_close(dec);
            return required ? 1 : AVB_TEST_SKIP;
        }
        auto *texture =
            static_cast<ID3D11Texture2D *>(frame.native_handle);
        ComPtr<ID3D11Device> texture_device;
        texture->GetDevice(&texture_device);
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_NV12 ||
            (int)desc.Width != frame.width ||
            (int)desc.Height != frame.height ||
            frame.native_handle_id >= desc.ArraySize ||
            (expected_device && texture_device.Get() != expected_device)) {
            std::fprintf(stderr,
                         "native %s decoder returned mismatched texture metadata\n",
                         label);
            avb_decoder_release_video_frame(dec, &frame);
            avb_decoder_close(dec);
            return required ? 1 : AVB_TEST_SKIP;
        }
        avb_decoder_release_video_frame(dec, &frame);
        if (pass == 0 && avb_decoder_seek(dec, 0.0, nullptr) != AVB_OK) {
            std::fprintf(stderr, "native %s decoder seek failed\n", label);
            avb_decoder_close(dec);
            return 1;
        }
    }

    avb_decoder_close(dec);
    std::printf(
        "Media Foundation native D3D11 %s Source Reader decode smoke passed\n",
        label);
    return 0;
}

static int smoke_native_multi_frame_hold(const char *input_path,
                                         const char *label) {
    ComPtr<ID3D11Device> device;
    D3D_FEATURE_LEVEL feature_level{};
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
        D3D11_SDK_VERSION, &device, &feature_level, nullptr);
    if (FAILED(hr)) {
        std::fprintf(stderr, "create %s multi-frame device failed\n", label);
        return 1;
    }

    avb_decode_options opts = avb_decode_options_default();
    opts.backend = AVB_BACKEND_MEDIAFOUNDATION;
    opts.enable_audio = 0;
    opts.enable_video = 1;
    opts.video_format = AVB_PIXEL_FORMAT_NV12;
    opts.video_memory = AVB_VIDEO_MEMORY_EXTERNAL;
    opts.video_external_type = AVB_VIDEO_EXTERNAL_D3D11_TEXTURE;
    opts.hardware_policy = AVB_HARDWARE_REQUIRE;
    opts.hardware_device = AVB_HW_DEVICE_D3D11VA;
    opts.hardware_context = device.Get();

    avb_decoder *dec = nullptr;
    if (avb_decoder_open(&dec, input_path, &opts) != AVB_OK) {
        std::fprintf(stderr, "open %s multi-frame decoder failed: %s\n",
                     label, avb_decoder_get_last_error(dec)
                        ? avb_decoder_get_last_error(dec) : "unknown");
        avb_decoder_close(dec);
        return 1;
    }

    avb_video_frame frames[3]{};
    for (int i = 0; i < 3; ++i) {
        if (avb_decoder_read_video_frame(dec, &frames[i]) != AVB_OK ||
            !frames[i].native_handle || !frames[i].native_owner) {
            std::fprintf(stderr, "%s multi-frame read %d failed\n", label, i);
            for (int j = 0; j <= i; ++j)
                avb_decoder_release_video_frame(dec, &frames[j]);
            avb_decoder_close(dec);
            return 1;
        }
        for (int j = 0; j < i; ++j) {
            if (frames[i].native_owner == frames[j].native_owner) {
                std::fprintf(stderr, "%s reused an active frame lease\n", label);
                for (int k = 0; k <= i; ++k)
                    avb_decoder_release_video_frame(dec, &frames[k]);
                avb_decoder_close(dec);
                return 1;
            }
        }
    }

    if (avb_decoder_seek(dec, 0.0, nullptr) != AVB_OK) {
        std::fprintf(stderr, "%s seek with held frames failed\n", label);
        for (auto &frame : frames)
            avb_decoder_release_video_frame(dec, &frame);
        avb_decoder_close(dec);
        return 1;
    }

    for (int i = 2; i >= 0; --i) {
        auto *texture =
            static_cast<ID3D11Texture2D *>(frames[i].native_handle);
        ComPtr<ID3D11Device> texture_device;
        texture->GetDevice(&texture_device);
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (texture_device.Get() != device.Get() ||
            desc.Format != DXGI_FORMAT_NV12) {
            std::fprintf(stderr, "%s held texture became invalid\n", label);
            for (int j = i; j >= 0; --j)
                avb_decoder_release_video_frame(dec, &frames[j]);
            avb_decoder_close(dec);
            return 1;
        }
        avb_decoder_release_video_frame(dec, &frames[i]);
    }

    avb_video_frame after_seek{};
    if (avb_decoder_read_video_frame(dec, &after_seek) != AVB_OK) {
        std::fprintf(stderr, "%s read after held-frame seek failed\n", label);
        avb_decoder_close(dec);
        return 1;
    }
    avb_decoder_release_video_frame(dec, &after_seek);
    avb_decoder_close(dec);
    std::printf(
        "Media Foundation native D3D11 %s multi-frame hold passed\n", label);
    return 0;
}

static int smoke_native_mp4_roundtrip(const char *input_path,
                                      const char *output_path,
                                      avb_video_codec output_codec,
                                      const char *label) {
    avb_decode_options dopts = avb_decode_options_default();
    dopts.backend = AVB_BACKEND_MEDIAFOUNDATION;
    dopts.enable_audio = 0;
    dopts.enable_video = 1;
    dopts.video_format = AVB_PIXEL_FORMAT_NV12;
    dopts.video_memory = AVB_VIDEO_MEMORY_EXTERNAL;
    dopts.video_external_type = AVB_VIDEO_EXTERNAL_D3D11_TEXTURE;
    dopts.hardware_policy = AVB_HARDWARE_REQUIRE;
    dopts.hardware_device = AVB_HW_DEVICE_D3D11VA;

    ComPtr<ID3D11Device> device;
    D3D_FEATURE_LEVEL feature_level{};
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
        D3D11_SDK_VERSION, &device, &feature_level, nullptr);
    if (FAILED(hr)) {
        std::fprintf(stderr,
                     "create native roundtrip D3D11 device failed: 0x%08lx\n",
                     hr);
        return 1;
    }
    dopts.hardware_context = device.Get();

    avb_decoder *dec = nullptr;
    if (avb_decoder_open(&dec, input_path, &dopts) != AVB_OK) {
        std::fprintf(stderr, "open native roundtrip decoder failed: %s\n",
                     avb_decoder_get_last_error(dec)
                        ? avb_decoder_get_last_error(dec) : "unknown");
        avb_decoder_close(dec);
        return 1;
    }

    avb_video_frame frame{};
    if (avb_decoder_read_video_frame(dec, &frame) != AVB_OK ||
        !frame.native_handle) {
        std::fprintf(stderr, "native roundtrip decoder produced no frame\n");
        avb_decoder_close(dec);
        return 1;
    }
    auto *texture = static_cast<ID3D11Texture2D *>(frame.native_handle);
    ComPtr<ID3D11Device> texture_device;
    texture->GetDevice(&texture_device);
    if (texture_device.Get() != device.Get()) {
        std::fprintf(stderr,
                     "native roundtrip decoder ignored the requested D3D11 device\n");
        avb_decoder_release_video_frame(dec, &frame);
        avb_decoder_close(dec);
        return 1;
    }

    avb_media_info info{};
    avb_decoder_get_media_info(dec, &info);
    avb_encode_options eopts = avb_encode_options_default();
    eopts.backend = AVB_BACKEND_MEDIAFOUNDATION;
    eopts.video.enable = 1;
    eopts.video.width = frame.width;
    eopts.video.height = frame.height;
    eopts.video.frame_rate =
        info.video.frame_rate > 0.0 ? info.video.frame_rate : 30.0;
    eopts.video.codec = output_codec;
    eopts.video.input_format = AVB_PIXEL_FORMAT_NV12;
    eopts.video.input_memory = AVB_VIDEO_MEMORY_EXTERNAL;
    eopts.video.input_external_type =
        AVB_VIDEO_EXTERNAL_D3D11_TEXTURE;
    eopts.video.hardware_policy = AVB_HARDWARE_REQUIRE;
    eopts.video.hardware_device = AVB_HW_DEVICE_D3D11VA;
    eopts.video.hardware_context = device.Get();

    avb_encoder *enc = nullptr;
    if (avb_encoder_open(&enc, output_path, &eopts) != AVB_OK) {
        std::fprintf(stderr, "open native roundtrip encoder failed: %s\n",
                     avb_encoder_get_last_error(enc)
                        ? avb_encoder_get_last_error(enc) : "unknown");
        avb_decoder_release_video_frame(dec, &frame);
        avb_encoder_close(enc);
        avb_decoder_close(dec);
        return 1;
    }

    int frames = 0;
    for (;;) {
        avb_result write =
            avb_encoder_write_video(enc, &frame, frame.pts_sec);
        avb_decoder_release_video_frame(dec, &frame);
        if (write != AVB_OK) {
            std::fprintf(stderr, "native roundtrip write failed: %s\n",
                         avb_encoder_get_last_error(enc)
                            ? avb_encoder_get_last_error(enc) : "unknown");
            avb_encoder_close(enc);
            avb_decoder_close(dec);
            return 1;
        }
        ++frames;
        if (frames >= 7 ||
            avb_decoder_read_video_frame(dec, &frame) != AVB_OK) {
            break;
        }
    }

    if (avb_encoder_finish(enc) != AVB_OK) {
        std::fprintf(stderr, "native roundtrip finish failed: %s\n",
                     avb_encoder_get_last_error(enc)
                        ? avb_encoder_get_last_error(enc) : "unknown");
        avb_encoder_close(enc);
        avb_decoder_close(dec);
        return 1;
    }
    avb_encoder_close(enc);
    avb_decoder_close(dec);

    int decoded = smoke_video_decoder(
        output_path, output_codec, avb_video_codec_name(output_codec));
    if (decoded != 0) return decoded == AVB_TEST_SKIP ? 1 : decoded;
    std::printf(
        "Media Foundation native D3D11 %s MP4 roundtrip passed (%d frames)\n",
        label, frames);
    return 0;
}
#endif

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "Usage: %s <fixture.mp4> <out.mp4> "
                     "[audio audio-codec audio-file expected-name] "
                     "[video video-codec video-file expected-name]...\n", argv[0]);
        return 2;
    }

    if (!avb_backend_is_available(AVB_BACKEND_MEDIAFOUNDATION)) {
        std::printf("SKIP: Media Foundation backend is not built\n");
        return AVB_TEST_SKIP;
    }

    bool ran_smoke = false;
    std::string vp8_fixture;
    std::string vp9_fixture;
    std::string av1_fixture;
    for (int i = 3; i < argc; ) {
        if (i + 3 >= argc) {
            std::fprintf(stderr, "incomplete runtime codec smoke arguments\n");
            return 2;
        }
        if (std::strcmp(argv[i], "audio") == 0) {
            avb_audio_codec codec = AVB_AUDIO_CODEC_AUTO;
            if (avb_audio_codec_from_name(argv[i + 1], &codec) != AVB_OK) {
                std::fprintf(stderr, "unknown audio codec '%s'\n", argv[i + 1]);
                return 2;
            }
            int res = smoke_audio_decoder(argv[i + 2], codec, argv[i + 3]);
            if (res == 1) return 1;
            if (res == 0) ran_smoke = true;
            i += 4;
        } else if (std::strcmp(argv[i], "video") == 0) {
            avb_video_codec codec = AVB_VIDEO_CODEC_AUTO;
            if (avb_video_codec_from_name(argv[i + 1], &codec) != AVB_OK) {
                std::fprintf(stderr, "unknown video codec '%s'\n", argv[i + 1]);
                return 2;
            }
            int res = smoke_video_decoder(argv[i + 2], codec, argv[i + 3]);
            if (res == 1) return 1;
            if (res == 0) ran_smoke = true;
            if (codec == AVB_VIDEO_CODEC_VP8) vp8_fixture = argv[i + 2];
            if (codec == AVB_VIDEO_CODEC_VP9) vp9_fixture = argv[i + 2];
            if (codec == AVB_VIDEO_CODEC_AV1) av1_fixture = argv[i + 2];
            i += 4;
        } else {
            std::fprintf(stderr, "unknown smoke kind '%s'\n", argv[i]);
            return 2;
        }
    }

#ifdef _WIN32
    if (!vp8_fixture.empty()) {
        int native_res = smoke_native_source_decoder(
            vp8_fixture.c_str(), "VP8 WebM", false);
        if (native_res == 0) ran_smoke = true;
    }
    if (!vp9_fixture.empty()) {
        int native_res = smoke_native_source_decoder(
            vp9_fixture.c_str(), "VP9 WebM", false);
        if (native_res == 0) ran_smoke = true;
    }
    if (!av1_fixture.empty()) {
        int native_res = smoke_native_source_decoder(
            av1_fixture.c_str(), "AV1 Matroska", false);
        if (native_res == 0) ran_smoke = true;
    }
#endif

    std::string pcm_out = std::string(argv[2]) + ".pcm_s16.wav";
    int pcm_res = smoke_audio_encoder(argv[1], pcm_out.c_str(),
                                      AVB_AUDIO_CODEC_PCM_S16, "pcm",
                                      "PCM_S16 WAV");
    if (pcm_res == 1) return 1;
    if (pcm_res == 0) ran_smoke = true;

    std::string pcm_f32_out = std::string(argv[2]) + ".pcm_f32.wav";
    int pcm_f32_res = smoke_audio_encoder(argv[1], pcm_f32_out.c_str(),
                                          AVB_AUDIO_CODEC_PCM_F32, "pcm_f32",
                                          "PCM_F32 WAV");
    if (pcm_f32_res == 1) return 1;
    if (pcm_f32_res == 0) ran_smoke = true;

    std::string mp3_out = std::string(argv[2]) + ".mp3";
    int mp3_res = smoke_audio_encoder(argv[1], mp3_out.c_str(),
                                      AVB_AUDIO_CODEC_MP3, "mp3",
                                      "MP3");
    if (mp3_res == 1) return 1;
    if (mp3_res == 0) ran_smoke = true;

    std::string flac_out = std::string(argv[2]) + ".flac";
    int flac_res = smoke_audio_encoder(argv[1], flac_out.c_str(),
                                       AVB_AUDIO_CODEC_FLAC, "flac",
                                       "FLAC");
    if (flac_res == 1) return 1;
    if (flac_res == 0) ran_smoke = true;

    if (!vp8_fixture.empty()) {
        std::string vp8_out = std::string(argv[2]) + ".vp8.ivf";
        int vp8_res = smoke_ivf_video_encoder(
            vp8_fixture.c_str(), vp8_out.c_str(), AVB_VIDEO_CODEC_VP8,
            "VP80", "VP8");
        if (vp8_res == 1) return 1;
        if (vp8_res == 0) ran_smoke = true;
    }
    if (!vp9_fixture.empty()) {
        std::string vp9_out = std::string(argv[2]) + ".vp9.ivf";
        int vp9_res = smoke_ivf_video_encoder(
            vp9_fixture.c_str(), vp9_out.c_str(), AVB_VIDEO_CODEC_VP9,
            "VP90", "VP9");
        if (vp9_res == 1) return 1;
        if (vp9_res == 0) ran_smoke = true;
    }
    if (!av1_fixture.empty()) {
        std::string av1_out = std::string(argv[2]) + ".av1.ivf";
        int av1_res = smoke_ivf_video_encoder(
            av1_fixture.c_str(), av1_out.c_str(), AVB_VIDEO_CODEC_AV1,
            "AV01", "AV1");
        if (av1_res == 1) return 1;
        if (av1_res == 0) ran_smoke = true;
    }
#ifdef _WIN32
    {
        int native_res = smoke_native_source_decoder(argv[1], "H264 MP4");
        if (native_res != 0) return 1;
        ran_smoke = true;
        if (smoke_native_multi_frame_hold(argv[1], "H264 MP4") != 0)
            return 1;

        std::string native_h264_out =
            std::string(argv[2]) + ".native.h264.mp4";
        native_res = smoke_native_video_encoder(
            native_h264_out.c_str(), AVB_VIDEO_CODEC_H264, "H264 MP4");
        if (native_res == 1) return 1;
        if (native_res == 0) ran_smoke = true;

        std::string native_roundtrip_out =
            std::string(argv[2]) + ".native.roundtrip.h264.mp4";
        native_res = smoke_native_mp4_roundtrip(
            argv[1], native_roundtrip_out.c_str(),
            AVB_VIDEO_CODEC_H264, "H264");
        if (native_res != 0) return 1;
        ran_smoke = true;

        std::string native_hevc_out =
            std::string(argv[2]) + ".native.hevc.mp4";
        native_res = smoke_native_video_encoder(
            native_hevc_out.c_str(), AVB_VIDEO_CODEC_HEVC, "HEVC MP4");
        if (native_res == 1) return 1;
        if (native_res == 0) ran_smoke = true;

        std::string native_hevc_roundtrip_out =
            std::string(argv[2]) + ".native.roundtrip.hevc.mp4";
        native_res = smoke_native_mp4_roundtrip(
            argv[1], native_hevc_roundtrip_out.c_str(),
            AVB_VIDEO_CODEC_HEVC, "HEVC");
        if (native_res != 0) return 1;
        ran_smoke = true;

        std::string native_out =
            std::string(argv[2]) + ".native.av1.ivf";
        native_res = smoke_native_video_encoder(
            native_out.c_str(), AVB_VIDEO_CODEC_AV1, "AV1");
        if (native_res == 1) return 1;
        if (native_res == 0) {
            ran_smoke = true;
            native_res = smoke_native_ivf_decoder(
                native_out.c_str(), "AV1");
            if (native_res != 0) return 1;
            if (smoke_native_multi_frame_hold(
                    native_out.c_str(), "AV1 IVF") != 0) {
                return 1;
            }
        }
    }
#endif

    avb_encoder_capabilities caps{};
    if (avb_encoder_probe_runtime_capabilities(
            AVB_BACKEND_MEDIAFOUNDATION, argv[2], &caps) != AVB_OK ||
        caps.result == AVB_ERROR_BACKEND_NOT_AVAILABLE) {
        std::printf("SKIP: Media Foundation runtime is not available\n");
        return ran_smoke ? 0 : AVB_TEST_SKIP;
    }
    if (caps.result != AVB_OK || !has_video_codec(caps, AVB_VIDEO_CODEC_HEVC)) {
        std::printf("SKIP: HEVC encoder MFT is not available\n");
        return ran_smoke ? 0 : AVB_TEST_SKIP;
    }

    avb_decode_options dopts = avb_decode_options_default();
    dopts.backend = AVB_BACKEND_MEDIAFOUNDATION;
    dopts.enable_audio = 0;
    dopts.enable_video = 1;
    dopts.video_format = AVB_PIXEL_FORMAT_BGRA8;

    avb_decoder *dec = nullptr;
    if (avb_decoder_open(&dec, argv[1], &dopts) != AVB_OK) {
        std::fprintf(stderr, "open input failed: %s\n",
                     avb_decoder_get_last_error(dec)
                        ? avb_decoder_get_last_error(dec) : "unknown");
        avb_decoder_close(dec);
        return 1;
    }

    avb_media_info info{};
    avb_decoder_get_media_info(dec, &info);

    avb_encode_options eopts = avb_encode_options_default();
    eopts.backend = AVB_BACKEND_MEDIAFOUNDATION;
    eopts.video.enable = 1;
    eopts.video.width = info.video.width;
    eopts.video.height = info.video.height;
    eopts.video.frame_rate = info.video.frame_rate > 0.0 ? info.video.frame_rate : 25.0;
    eopts.video.codec = AVB_VIDEO_CODEC_HEVC;
    eopts.video.input_format = AVB_PIXEL_FORMAT_BGRA8;
    eopts.video.bitrate = 1500000;

    avb_encoder *enc = nullptr;
    if (avb_encoder_open(&enc, argv[2], &eopts) != AVB_OK) {
        std::printf("SKIP: HEVC encoder MFT was listed but this media sink "
                    "did not accept the test session: %s\n",
                    avb_encoder_get_last_error(enc)
                        ? avb_encoder_get_last_error(enc) : "unknown");
        avb_encoder_close(enc);
        avb_decoder_close(dec);
        return AVB_TEST_SKIP;
    }

    int frames = 0;
    avb_video_frame frame{};
    while (frames < 30 && avb_decoder_read_video_frame(dec, &frame) == AVB_OK) {
        avb_result wr = avb_encoder_write_video(enc, &frame, frame.pts_sec);
        avb_decoder_release_video_frame(dec, &frame);
        if (wr != AVB_OK) {
            std::fprintf(stderr, "write HEVC frame failed: %s\n",
                         avb_encoder_get_last_error(enc)
                            ? avb_encoder_get_last_error(enc) : "unknown");
            avb_encoder_close(enc);
            avb_decoder_close(dec);
            return 1;
        }
        ++frames;
    }

    avb_result finish = avb_encoder_finish(enc);
    if (finish != AVB_OK) {
        std::fprintf(stderr, "finish HEVC encode failed: %s\n",
                     avb_encoder_get_last_error(enc)
                        ? avb_encoder_get_last_error(enc) : "unknown");
        avb_encoder_close(enc);
        avb_decoder_close(dec);
        return 1;
    }

    avb_encoder_close(enc);
    avb_decoder_close(dec);

    std::string codec = probe_video_codec(argv[2]);
    if (codec != "hevc") {
        std::fprintf(stderr, "expected hevc output, got '%s'\n", codec.c_str());
        return 1;
    }

#ifdef _WIN32
    if (smoke_native_source_decoder(argv[2], "HEVC MP4") != 0)
        return 1;
#endif

    std::printf("Media Foundation HEVC runtime encode smoke passed (%d frames)\n",
                frames);
    return 0;
}
