# Conformance test. The fixture is generated with ffmpeg at build time so the
# repository stays free of binary blobs; if ffmpeg is unavailable the test is
# skipped (registered but reported as "not run") rather than failing the build.

add_executable(avb_conformance conformance.cpp)
target_link_libraries(avb_conformance PRIVATE avbridge)
if(AVB_BUILD_SHARED)
    target_compile_definitions(avb_conformance PRIVATE AVB_USING_SHARED)
endif()

add_executable(avb_roundtrip roundtrip.cpp)
target_link_libraries(avb_roundtrip PRIVATE avbridge)
if(AVB_BUILD_SHARED)
    target_compile_definitions(avb_roundtrip PRIVATE AVB_USING_SHARED)
endif()

add_executable(avb_codecs codecs.cpp)
target_link_libraries(avb_codecs PRIVATE avbridge)
if(AVB_BUILD_SHARED)
    target_compile_definitions(avb_codecs PRIVATE AVB_USING_SHARED)
endif()

add_executable(avb_io io.cpp)
target_link_libraries(avb_io PRIVATE avbridge)
if(AVB_BUILD_SHARED)
    target_compile_definitions(avb_io PRIVATE AVB_USING_SHARED)
endif()

add_executable(avb_custom_decoder custom_decoder.cpp)
target_link_libraries(avb_custom_decoder PRIVATE avbridge)
if(AVB_BUILD_SHARED)
    target_compile_definitions(avb_custom_decoder PRIVATE AVB_USING_SHARED)
endif()

add_executable(avb_custom_encoder custom_encoder.cpp)
target_link_libraries(avb_custom_encoder PRIVATE avbridge)
if(AVB_BUILD_SHARED)
    target_compile_definitions(avb_custom_encoder PRIVATE AVB_USING_SHARED)
endif()

add_executable(avb_mediafoundation_runtime_codecs mediafoundation_runtime_codecs.cpp)
target_link_libraries(avb_mediafoundation_runtime_codecs PRIVATE avbridge)
if(WIN32)
    target_link_libraries(avb_mediafoundation_runtime_codecs PRIVATE d3d11 dxgi)
endif()
if(AVB_BUILD_SHARED)
    target_compile_definitions(avb_mediafoundation_runtime_codecs PRIVATE AVB_USING_SHARED)
endif()

add_executable(avb_dmabuf_roundtrip dmabuf_roundtrip.cpp)
target_link_libraries(avb_dmabuf_roundtrip PRIVATE avbridge)
if(AVB_BUILD_SHARED)
    target_compile_definitions(avb_dmabuf_roundtrip PRIVATE AVB_USING_SHARED)
endif()

find_program(FFMPEG_EXECUTABLE ffmpeg)

if(FFMPEG_EXECUTABLE)
    set(AVB_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/fixtures/conformance.mp4")
    set(AVB_OUT "${CMAKE_CURRENT_BINARY_DIR}/fixtures")

    add_custom_command(
        OUTPUT "${AVB_FIXTURE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${AVB_OUT}"
        COMMAND ${FFMPEG_EXECUTABLE} -y -loglevel error
                -f lavfi -i "testsrc=duration=3:size=320x240:rate=25"
                -f lavfi -i "sine=frequency=440:duration=3"
                -c:v libx264 -c:a aac -pix_fmt yuv420p -ar 44100 -ac 1
                "${AVB_FIXTURE}"
        COMMENT "Generating conformance fixture with ffmpeg"
        VERBATIM
    )
    add_custom_target(avb_conformance_fixture DEPENDS "${AVB_FIXTURE}")
    set(AVB_MF_RUNTIME_CODEC_ARGS)
    if(WIN32 AND AVB_ENABLE_MEDIAFOUNDATION)
        execute_process(
            COMMAND ${FFMPEG_EXECUTABLE} -hide_banner -encoders
            OUTPUT_VARIABLE AVB_FFMPEG_ENCODERS
            ERROR_QUIET
        )

        set(AVB_MF_AUDIO_FIXTURES)
        set(AVB_MF_VIDEO_FIXTURES)
        set(AVB_MF_FLAC_FIXTURE "${AVB_OUT}/runtime_codecs_mediafoundation_flac.flac")
        add_custom_command(
            OUTPUT "${AVB_MF_FLAC_FIXTURE}"
            COMMAND ${FFMPEG_EXECUTABLE} -y -loglevel error
                    -f lavfi -i "sine=frequency=660:duration=1"
                    -c:a flac -ar 44100 -ac 1
                    "${AVB_MF_FLAC_FIXTURE}"
            COMMENT "Generating Media Foundation FLAC runtime fixture"
            VERBATIM
        )
        list(APPEND AVB_MF_AUDIO_FIXTURES "${AVB_MF_FLAC_FIXTURE}")
        list(APPEND AVB_MF_RUNTIME_CODEC_ARGS
             audio flac "${AVB_MF_FLAC_FIXTURE}" flac)

        if(AVB_FFMPEG_ENCODERS MATCHES "libopus")
            set(AVB_MF_OPUS_FIXTURE "${AVB_OUT}/runtime_codecs_mediafoundation_opus.ogg")
            add_custom_command(
                OUTPUT "${AVB_MF_OPUS_FIXTURE}"
                COMMAND ${FFMPEG_EXECUTABLE} -y -loglevel error
                        -f lavfi -i "sine=frequency=550:duration=1"
                        -c:a libopus -ar 48000 -ac 1
                        "${AVB_MF_OPUS_FIXTURE}"
                COMMENT "Generating Media Foundation Opus runtime fixture"
                VERBATIM
            )
            list(APPEND AVB_MF_AUDIO_FIXTURES "${AVB_MF_OPUS_FIXTURE}")
            list(APPEND AVB_MF_RUNTIME_CODEC_ARGS
                 audio opus "${AVB_MF_OPUS_FIXTURE}" opus)
        endif()

        if(AVB_FFMPEG_ENCODERS MATCHES "libvorbis")
            set(AVB_MF_VORBIS_FIXTURE "${AVB_OUT}/runtime_codecs_mediafoundation_vorbis.ogg")
            add_custom_command(
                OUTPUT "${AVB_MF_VORBIS_FIXTURE}"
                COMMAND ${FFMPEG_EXECUTABLE} -y -loglevel error
                        -f lavfi -i "sine=frequency=770:duration=1"
                        -c:a libvorbis -ar 44100 -ac 1
                        "${AVB_MF_VORBIS_FIXTURE}"
                COMMENT "Generating Media Foundation Vorbis runtime fixture"
                VERBATIM
            )
            list(APPEND AVB_MF_AUDIO_FIXTURES "${AVB_MF_VORBIS_FIXTURE}")
            list(APPEND AVB_MF_RUNTIME_CODEC_ARGS
                 audio vorbis "${AVB_MF_VORBIS_FIXTURE}" "*")
        endif()

        if(AVB_FFMPEG_ENCODERS MATCHES "libvpx")
            set(AVB_MF_VP8_FIXTURE "${AVB_OUT}/runtime_codecs_mediafoundation_vp8.webm")
            add_custom_command(
                OUTPUT "${AVB_MF_VP8_FIXTURE}"
                COMMAND ${FFMPEG_EXECUTABLE} -y -loglevel error
                        -f lavfi -i "testsrc=duration=1:size=160x120:rate=15"
                        -an -c:v libvpx -pix_fmt yuv420p -b:v 500k
                        "${AVB_MF_VP8_FIXTURE}"
                COMMENT "Generating Media Foundation VP8 runtime fixture"
                VERBATIM
            )
            list(APPEND AVB_MF_VIDEO_FIXTURES "${AVB_MF_VP8_FIXTURE}")
            list(APPEND AVB_MF_RUNTIME_CODEC_ARGS
                 video vp8 "${AVB_MF_VP8_FIXTURE}" vp8)
        endif()

        if(AVB_FFMPEG_ENCODERS MATCHES "libvpx-vp9")
            set(AVB_MF_VP9_FIXTURE "${AVB_OUT}/runtime_codecs_mediafoundation_vp9.webm")
            add_custom_command(
                OUTPUT "${AVB_MF_VP9_FIXTURE}"
                COMMAND ${FFMPEG_EXECUTABLE} -y -loglevel error
                        -f lavfi -i "testsrc=duration=1:size=160x120:rate=15"
                        -an -c:v libvpx-vp9 -pix_fmt yuv420p -b:v 500k
                        "${AVB_MF_VP9_FIXTURE}"
                COMMENT "Generating Media Foundation VP9 runtime fixture"
                VERBATIM
            )
            list(APPEND AVB_MF_VIDEO_FIXTURES "${AVB_MF_VP9_FIXTURE}")
            list(APPEND AVB_MF_RUNTIME_CODEC_ARGS
                 video vp9 "${AVB_MF_VP9_FIXTURE}" vp9)
        endif()

        if(AVB_FFMPEG_ENCODERS MATCHES "libaom-av1")
            set(AVB_MF_AV1_FIXTURE "${AVB_OUT}/runtime_codecs_mediafoundation_av1.mkv")
            add_custom_command(
                OUTPUT "${AVB_MF_AV1_FIXTURE}"
                COMMAND ${FFMPEG_EXECUTABLE} -y -loglevel error
                        -f lavfi -i "testsrc=duration=1:size=320x240:rate=15"
                        -an -c:v libaom-av1 -pix_fmt yuv420p -crf 38 -b:v 0
                        -cpu-used 8
                        "${AVB_MF_AV1_FIXTURE}"
                COMMENT "Generating Media Foundation AV1 runtime fixture"
                VERBATIM
            )
            list(APPEND AVB_MF_VIDEO_FIXTURES "${AVB_MF_AV1_FIXTURE}")
            list(APPEND AVB_MF_RUNTIME_CODEC_ARGS
                 video av1 "${AVB_MF_AV1_FIXTURE}" av1)
        endif()

        if(AVB_MF_AUDIO_FIXTURES OR AVB_MF_VIDEO_FIXTURES)
            add_custom_target(avb_mediafoundation_codec_fixtures
                              DEPENDS ${AVB_MF_AUDIO_FIXTURES}
                                      ${AVB_MF_VIDEO_FIXTURES})
        endif()
    endif()

    add_dependencies(avb_conformance avb_conformance_fixture)
    add_dependencies(avb_roundtrip avb_conformance_fixture)
    add_dependencies(avb_codecs avb_conformance_fixture)
    add_dependencies(avb_io avb_conformance_fixture)
    add_dependencies(avb_custom_decoder avb_conformance_fixture)
    add_dependencies(avb_custom_encoder avb_conformance_fixture)
    add_dependencies(avb_mediafoundation_runtime_codecs avb_conformance_fixture)
    if(TARGET avb_mediafoundation_codec_fixtures)
        add_dependencies(avb_mediafoundation_runtime_codecs
                         avb_mediafoundation_codec_fixtures)
    endif()
    add_dependencies(avb_dmabuf_roundtrip avb_conformance_fixture)

    # The conformance contract is backend-agnostic, so run it against every
    # backend compiled into this build (each skips itself if not present). A
    # SKIP_RETURN_CODE of 77 reports "not run" rather than a failure.
    set(_avb_backends auto)
    if(WIN32 AND AVB_ENABLE_MEDIAFOUNDATION)
        list(APPEND _avb_backends mediafoundation)
    endif()
    if(AVB_ENABLE_GSTREAMER)
        list(APPEND _avb_backends gstreamer)
    endif()
    if(AVB_ENABLE_FFMPEG)
        list(APPEND _avb_backends ffmpeg)
    endif()
    foreach(be ${_avb_backends})
        add_test(NAME conformance_${be} COMMAND avb_conformance "${AVB_FIXTURE}" ${be})
        add_test(NAME roundtrip_${be}
                 COMMAND avb_roundtrip "${AVB_FIXTURE}" "${AVB_OUT}/roundtrip_${be}.mp4" ${be})
        add_test(NAME io_${be} COMMAND avb_io "${AVB_FIXTURE}" ${be})
        set_tests_properties(conformance_${be} roundtrip_${be} io_${be}
                             PROPERTIES SKIP_RETURN_CODE 77)
    endforeach()

    # Wide codec/pixel-format coverage via the FFmpeg backend (skips if absent).
    add_test(NAME codecs_ffmpeg
             COMMAND avb_codecs "${AVB_FIXTURE}" "${AVB_OUT}/codec_ffmpeg" ffmpeg)
    set_tests_properties(codecs_ffmpeg PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME custom_decoder_ffmpeg
             COMMAND avb_custom_decoder "${AVB_FIXTURE}" ffmpeg)
    set_tests_properties(custom_decoder_ffmpeg PROPERTIES SKIP_RETURN_CODE 77)
    if(WIN32 AND AVB_ENABLE_MEDIAFOUNDATION)
        add_test(NAME custom_decoder_mediafoundation
                 COMMAND avb_custom_decoder "${AVB_FIXTURE}" mediafoundation)
        set_tests_properties(custom_decoder_mediafoundation PROPERTIES SKIP_RETURN_CODE 77)
        add_test(NAME runtime_codecs_mediafoundation
                 COMMAND avb_mediafoundation_runtime_codecs "${AVB_FIXTURE}"
                         "${AVB_OUT}/runtime_codecs_mediafoundation_hevc.mp4"
                         ${AVB_MF_RUNTIME_CODEC_ARGS})
        set_tests_properties(runtime_codecs_mediafoundation PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    add_test(NAME custom_encoder_ffmpeg
             COMMAND avb_custom_encoder "${AVB_OUT}/custom_encoder_ffmpeg.mov" ffmpeg)
    set_tests_properties(custom_encoder_ffmpeg PROPERTIES SKIP_RETURN_CODE 77)
    if(AVB_ENABLE_FFMPEG)
        add_test(NAME dmabuf_roundtrip_ffmpeg
                 COMMAND avb_dmabuf_roundtrip "${AVB_FIXTURE}"
                         "${AVB_OUT}/dmabuf_roundtrip_ffmpeg.mp4" ffmpeg ffmpeg)
        set_tests_properties(dmabuf_roundtrip_ffmpeg PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    if(AVB_ENABLE_GSTREAMER)
        add_test(NAME codecs_gstreamer
                 COMMAND avb_codecs "${AVB_FIXTURE}" "${AVB_OUT}/codec_gstreamer" gstreamer video-only)
        set_tests_properties(codecs_gstreamer PROPERTIES SKIP_RETURN_CODE 77)
        add_test(NAME codecs_gstreamer_audio
                 COMMAND avb_codecs "${AVB_FIXTURE}" "${AVB_OUT}/codec_gstreamer_audio" gstreamer audio-only)
        set_tests_properties(codecs_gstreamer_audio PROPERTIES SKIP_RETURN_CODE 77)
        add_test(NAME custom_decoder_gstreamer
                 COMMAND avb_custom_decoder "${AVB_FIXTURE}" gstreamer)
        set_tests_properties(custom_decoder_gstreamer PROPERTIES SKIP_RETURN_CODE 77)
        add_test(NAME custom_encoder_gstreamer
                 COMMAND avb_custom_encoder "${AVB_OUT}/custom_encoder_gstreamer.mkv" gstreamer)
        set_tests_properties(custom_encoder_gstreamer PROPERTIES SKIP_RETURN_CODE 77)
        add_test(NAME dmabuf_roundtrip_gstreamer
                 COMMAND avb_dmabuf_roundtrip "${AVB_FIXTURE}"
                         "${AVB_OUT}/dmabuf_roundtrip_gstreamer.mp4" gstreamer gstreamer)
        set_tests_properties(dmabuf_roundtrip_gstreamer PROPERTIES SKIP_RETURN_CODE 77)
        if(AVB_ENABLE_FFMPEG)
            add_test(NAME dmabuf_roundtrip_ffmpeg_to_gstreamer
                     COMMAND avb_dmabuf_roundtrip "${AVB_FIXTURE}"
                             "${AVB_OUT}/dmabuf_roundtrip_ffmpeg_to_gstreamer.mp4" ffmpeg gstreamer)
            set_tests_properties(dmabuf_roundtrip_ffmpeg_to_gstreamer PROPERTIES SKIP_RETURN_CODE 77)
            add_test(NAME dmabuf_roundtrip_gstreamer_to_ffmpeg
                     COMMAND avb_dmabuf_roundtrip "${AVB_FIXTURE}"
                             "${AVB_OUT}/dmabuf_roundtrip_gstreamer_to_ffmpeg.mp4" gstreamer ffmpeg)
            set_tests_properties(dmabuf_roundtrip_gstreamer_to_ffmpeg PROPERTIES SKIP_RETURN_CODE 77)
        endif()
    endif()
    if(AVB_ENABLE_AVFOUNDATION)
        add_test(NAME custom_decoder_avfoundation
                 COMMAND avb_custom_decoder "${AVB_FIXTURE}" avfoundation)
        set_tests_properties(custom_decoder_avfoundation PROPERTIES SKIP_RETURN_CODE 77)
        add_test(NAME custom_encoder_avfoundation
                 COMMAND avb_custom_encoder "${AVB_OUT}/custom_encoder_avfoundation.mov" avfoundation)
        set_tests_properties(custom_encoder_avfoundation PROPERTIES SKIP_RETURN_CODE 77)
    endif()
else()
    message(WARNING
        "ffmpeg not found; the conformance test fixture cannot be generated. "
        "Install ffmpeg to enable the test suite.")
endif()
