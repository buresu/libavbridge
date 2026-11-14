#include "test.hpp"
#include "backends/mediafoundation/avb_mediafoundation_network.hpp"
#include <cstdlib>
#include <string>

// A URL's user and password come out of it for the network source's
// credential manager. With AVB_RTSP_URL naming a stream behind
// authentication (rtsp://user:password@host:port/path), the stream is opened
// and a picture read through it; with AVB_RTSP_BAD_URL, one with a wrong
// password, the open fails rather than hangs.
int main() {
    avb::test::Context test;

    test.section("url credentials");
    {
        const auto c = mf_split_url_credentials(
            L"rtsp://root:pa%40ss@10.10.10.2:554/axis-media/media.amp?fps=15");
        test.check(c.present, "a user is found");
        test.check(c.user == L"root", "the user");
        test.check(c.password == L"pa@ss", "the password, percent-decoded");
        test.check(c.url == L"rtsp://10.10.10.2:554/axis-media/media.amp?fps=15",
                   "the URL without them");
    }
    {
        const auto c = mf_split_url_credentials(L"rtsp://viewer@host/stream");
        test.check(c.present && c.user == L"viewer" && c.password.empty(),
                   "a user with no password");
        test.check(c.url == L"rtsp://host/stream", "the URL without the user");
    }
    {
        // An '@' after the host is the path's, not a user's.
        const auto c = mf_split_url_credentials(L"http://host/a@b");
        test.check(!c.present, "no user before the host");
        test.check(c.url == L"http://host/a@b", "the URL as it was");
    }
    for (const wchar_t *plain :
         {L"C:\\movies\\gate@2.mp4", L"rtsp://host:554/stream", L""}) {
        const auto c = mf_split_url_credentials(plain);
        test.check(!c.present && c.url == plain, "no user: as it was");
    }

    const char *url = std::getenv("AVB_RTSP_URL");
    const char *bad = std::getenv("AVB_RTSP_BAD_URL");
    if (url && *url) {
        test.section("live stream");
        avb_decode_options options = avb_decode_options_default();
        options.backend = AVB_BACKEND_MEDIAFOUNDATION;
        options.enable_audio = 0;
        options.video_format = AVB_PIXEL_FORMAT_BGRA8;
        avb_decoder *decoder = nullptr;
        const avb_result opened = avb_decoder_open(&decoder, url, &options);
        if (opened != AVB_OK && decoder)
            std::fprintf(stderr, "open: %s\n",
                         avb_decoder_get_last_error(decoder));
        test.equal(opened, AVB_OK, "the stream opens with its user");
        if (opened == AVB_OK) {
            avb_video_frame frame{};
            test.equal(avb_decoder_read_video_frame(decoder, &frame), AVB_OK,
                       "a picture is read");
            test.check(frame.width > 0 && frame.height > 0,
                       "the picture has a size");
        }
        if (decoder) avb_decoder_close(decoder);
    }
    if (bad && *bad) {
        test.section("wrong password");
        avb_decode_options options = avb_decode_options_default();
        options.backend = AVB_BACKEND_MEDIAFOUNDATION;
        options.enable_audio = 0;
        avb_decoder *decoder = nullptr;
        const avb_result opened = avb_decoder_open(&decoder, bad, &options);
        test.check(opened != AVB_OK, "the open fails");
        if (decoder) {
            const char *error = avb_decoder_get_last_error(decoder);
            std::printf("refused: %s\n", error ? error : "(none)");
            avb_decoder_close(decoder);
        }
    }
    return test.finish("mediafoundation_network");
}
