#include <avbridge.h>

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(int condition, const char *message) {
  if (condition)
    return;
  fprintf(stderr, "FAIL: %s\n", message);
  ++failures;
}

int main(void) {
  avb_decode_options decode = avb_decode_options_default();
  avb_encode_options encode = avb_encode_options_default();
  avb_backend backend = AVB_BACKEND_COUNT;
  avb_video_codec video_codec = AVB_VIDEO_CODEC_COUNT;
  avb_audio_codec audio_codec = AVB_AUDIO_CODEC_COUNT;

  check(decode.backend == AVB_BACKEND_AUTO, "C decode default backend");
  check(decode.enable_audio == 1, "C decode default audio");
  check(decode.enable_video == 1, "C decode default video");
  check(encode.backend == AVB_BACKEND_AUTO, "C encode default backend");
  check(encode.video.enable == 0, "C encode default video");
  check(encode.audio.enable == 0, "C encode default audio");

  check(avb_backend_from_name("ffmpeg", &backend) == AVB_OK &&
            backend == AVB_BACKEND_FFMPEG,
        "C backend parser");
  check(avb_video_codec_from_name("h264", &video_codec) == AVB_OK &&
            video_codec == AVB_VIDEO_CODEC_H264,
        "C video codec parser");
  check(avb_audio_codec_from_name("aac", &audio_codec) == AVB_OK &&
            audio_codec == AVB_AUDIO_CODEC_AAC,
        "C audio codec parser");
  check(strcmp(avb_result_string(AVB_ERROR_EOF), "AVB_ERROR_EOF") == 0,
        "C result string");

  avb_decoder_close(NULL);
  avb_encoder_close(NULL);

  if (failures != 0) {
    fprintf(stderr, "c_api_contract: %d failure(s)\n", failures);
    return 1;
  }
  printf("c_api_contract: passed\n");
  return 0;
}
