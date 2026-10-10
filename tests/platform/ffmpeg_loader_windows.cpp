#include "test.hpp"
#include "backends/ffmpeg/avb_ffmpeg_loader.hpp"

#include <windows.h>
#include <atomic>
#include <filesystem>
#include <thread>
#include <vector>

using namespace avb::test;
namespace fs = std::filesystem;

namespace {
constexpr const char *format_dll = "avformat-" AV_STRINGIFY(LIBAVFORMAT_VERSION_MAJOR) ".dll";
constexpr const char *scale_dll = "swscale-" AV_STRINGIFY(LIBSWSCALE_VERSION_MAJOR) ".dll";

void link_or_copy(const fs::path &source, const fs::path &dest) {
    std::error_code error;
    fs::create_hard_link(source, dest, error);
    if (error) fs::copy_file(source, dest);
}

bool load(Context &test, const char *message) {
    AvbFFmpegFuncs functions{};
    char error[512]{};
    const bool loaded = avb_ffmpeg_load(functions, error, sizeof(error));
    if (!loaded) std::fprintf(stderr, "%s\n", error);
    test.check(loaded, message);
    if (loaded) {
        AVFrame *frame = functions.av_frame_alloc();
        test.check(frame != nullptr, "loaded functions are callable");
        functions.av_frame_free(&frame);
    }
    return loaded;
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    Context test;
    test.section("concurrent FFmpeg loading");
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < 20; ++j) {
                AvbFFmpegFuncs functions{};
                char error[512]{};
                if (!avb_ffmpeg_load(functions, error, sizeof(error))) {
                    ++errors;
                    continue;
                }
                AVFrame *frame = functions.av_frame_alloc();
                if (!frame) ++errors;
                functions.av_frame_free(&frame);
            }
        });
    }
    for (auto &thread : threads) thread.join();
    test.equal(errors.load(), 0, "concurrent loads return usable function tables");
    wchar_t runtime_path[32768]{};
    const HMODULE module = GetModuleHandleA(format_dll);
    if (!module || !GetModuleFileNameW(module, runtime_path, 32768)) return 1;
    const fs::path runtime = fs::path(runtime_path).parent_path();
    avb_ffmpeg_unload();
    test.check(GetModuleHandleA(format_dll) == nullptr, "repeated loads do not leak DLL references");
    // Continuing with leaked handles would mask the isolated directory tests.
    if (GetModuleHandleA(format_dll)) return test.finish("FFmpeg Windows loader");

    const fs::path scratch = fs::current_path() /
        (L"ffmpeg_loader_\u65e5\u672c\u8a9e_" + std::to_wstring(GetCurrentProcessId()));
    if (!fs::create_directory(scratch)) return 1;
    const fs::path bin = scratch / "bin";
    fs::create_directory(bin);
    for (const auto &entry : fs::directory_iterator(runtime)) {
        if (entry.path().extension() == L".dll")
            link_or_copy(entry.path(), bin / entry.path().filename());
    }
    wchar_t system[32768]{};
    GetSystemDirectoryW(system, 32768);
    SetEnvironmentVariableW(L"PATH", system);
    SetDllDirectoryW(L""); // Exclude the current directory from DLL search.
    const auto relative_root = fs::relative(scratch).wstring();
    SetEnvironmentVariableW(L"FFMPEG_DIR", relative_root.c_str());
    test.section("relative Unicode FFMPEG_DIR with adjacent dependencies");
    load(test, "relative package root loads bin DLLs");
    avb_ffmpeg_unload();
    SetEnvironmentVariableW(L"FFMPEG_DIR", fs::relative(bin).c_str());
    load(test, "relative DLL directory loads");
    avb_ffmpeg_unload();

    test.section("failed load cleanup and retry");
    // Remove only the link we created, never modify the installed FFmpeg DLL.
    fs::remove(bin / scale_dll);
    fs::copy_file(fs::u8path(argv[1]), bin / scale_dll);
    for (bool wrong_abi : {true, false}) {
        SetEnvironmentVariableW(L"AVB_TEST_WRONG_ABI", wrong_abi ? L"1" : nullptr);
        AvbFFmpegFuncs functions{};
        char error[512]{};
        test.check(!avb_ffmpeg_load(functions, error, sizeof(error)),
                   "incompatible or incomplete DLL is rejected");
        test.check(std::strstr(error, wrong_abi ? "ABI mismatch" : "sws_getContext") != nullptr,
                   "load error identifies the cause");
        test.check(!functions.avformat_open_input && !functions.av_frame_alloc,
                   "failed load returns no dangling function pointers");
        test.check(GetModuleHandleA(format_dll) == nullptr && GetModuleHandleA(scale_dll) == nullptr,
                   "failed load releases every DLL");
    }
    fs::remove(bin / scale_dll);
    link_or_copy(runtime / scale_dll, bin / scale_dll);
    load(test, "load recovers after failed attempts");
    avb_ffmpeg_unload();
    fs::remove_all(scratch);
    return test.finish("FFmpeg Windows loader");
}
