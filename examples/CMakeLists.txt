add_executable(avb_probe probe_media.cpp)
target_link_libraries(avb_probe PRIVATE avbridge)

add_executable(avb_decode_audio decode_audio.cpp)
target_link_libraries(avb_decode_audio PRIVATE avbridge)

add_executable(avb_decode_video decode_video.cpp)
target_link_libraries(avb_decode_video PRIVATE avbridge)

add_executable(avb_transcode transcode.cpp)
target_link_libraries(avb_transcode PRIVATE avbridge)
