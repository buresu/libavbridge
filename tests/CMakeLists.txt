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
    add_dependencies(avb_conformance avb_conformance_fixture)
    add_dependencies(avb_roundtrip avb_conformance_fixture)
    add_dependencies(avb_codecs avb_conformance_fixture)
    add_dependencies(avb_io avb_conformance_fixture)
    add_dependencies(avb_custom_decoder avb_conformance_fixture)
    add_dependencies(avb_custom_encoder avb_conformance_fixture)

    # The conformance contract is backend-agnostic, so run it against every
    # backend compiled into this build (each skips itself if not present). A
    # SKIP_RETURN_CODE of 77 reports "not run" rather than a failure.
    set(_avb_backends auto)
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
    add_test(NAME codecs COMMAND avb_codecs "${AVB_FIXTURE}" "${AVB_OUT}/codec")
    set_tests_properties(codecs PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME custom_decoder_ffmpeg
             COMMAND avb_custom_decoder "${AVB_FIXTURE}" ffmpeg)
    set_tests_properties(custom_decoder_ffmpeg PROPERTIES SKIP_RETURN_CODE 77)
    if(WIN32 AND AVB_ENABLE_MEDIAFOUNDATION)
        add_test(NAME custom_decoder_mediafoundation
                 COMMAND avb_custom_decoder "${AVB_FIXTURE}" mediafoundation)
        set_tests_properties(custom_decoder_mediafoundation PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    add_test(NAME custom_encoder_ffmpeg
             COMMAND avb_custom_encoder "${AVB_OUT}/custom_encoder_ffmpeg.mov" ffmpeg)
    set_tests_properties(custom_encoder_ffmpeg PROPERTIES SKIP_RETURN_CODE 77)
    if(AVB_ENABLE_GSTREAMER)
        add_test(NAME custom_decoder_gstreamer
                 COMMAND avb_custom_decoder "${AVB_FIXTURE}" gstreamer)
        set_tests_properties(custom_decoder_gstreamer PROPERTIES SKIP_RETURN_CODE 77)
        add_test(NAME custom_encoder_gstreamer
                 COMMAND avb_custom_encoder "${AVB_OUT}/custom_encoder_gstreamer.mkv" gstreamer)
        set_tests_properties(custom_encoder_gstreamer PROPERTIES SKIP_RETURN_CODE 77)
    endif()
else()
    message(WARNING
        "ffmpeg not found; the conformance test fixture cannot be generated. "
        "Install ffmpeg to enable the test suite.")
endif()
