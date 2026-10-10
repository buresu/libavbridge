# Tests are split into:
#   contract     - fixture-free public API/ABI checks
#   conformance  - the same media behavior against every enabled backend
#   integration  - codec, custom plugin, and native-memory round trips
#   platform     - native handle, descriptor, and runtime-specific contracts

function(avb_add_test_binary target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE avbridge)
    target_include_directories(
        ${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
    if(AVB_BUILD_SHARED)
        target_compile_definitions(${target} PRIVATE AVB_USING_SHARED)
    endif()
endfunction()

function(avb_add_ctest name target labels)
    add_test(NAME ${name} COMMAND ${target} ${ARGN})
    set_tests_properties(${name} PROPERTIES
        LABELS "${labels}"
        SKIP_RETURN_CODE 77
        TIMEOUT 180
    )
endfunction()

avb_add_test_binary(avb_api_contract contract/api.cpp)
avb_add_test_binary(avb_c_api_contract contract/c_api.c)
set_target_properties(avb_c_api_contract PROPERTIES LINKER_LANGUAGE CXX)
avb_add_test_binary(avb_conformance conformance/decode.cpp)
avb_add_test_binary(avb_audio_resample conformance/audio_resample.cpp)
avb_add_test_binary(avb_encoder_input conformance/encoder_input.cpp)
avb_add_test_binary(avb_odd_dimensions conformance/odd_dimensions.cpp)
target_include_directories(avb_odd_dimensions PRIVATE ${PROJECT_SOURCE_DIR}/src)
avb_add_test_binary(avb_io conformance/io.cpp)
if(AVB_ENABLE_FFMPEG AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    avb_add_test_binary(avb_ffmpeg_read_errors platform/ffmpeg_read_errors.cpp)
endif()
if(AVB_ENABLE_GSTREAMER)
    avb_add_test_binary(avb_stream_selection conformance/stream_selection.cpp)
    avb_add_test_binary(avb_gstreamer_errors platform/gstreamer_errors.cpp)
    target_include_directories(avb_gstreamer_errors PRIVATE ${GST_INCLUDE_DIRS})
    target_link_directories(avb_gstreamer_errors PRIVATE ${GST_LIBRARY_DIRS})
    target_link_libraries(avb_gstreamer_errors PRIVATE ${GST_LIBRARIES})
endif()
avb_add_test_binary(avb_roundtrip integration/roundtrip.cpp)
avb_add_test_binary(avb_codecs integration/codecs.cpp)
avb_add_test_binary(avb_custom_decoder integration/custom_decoder.cpp)
avb_add_test_binary(avb_custom_encoder integration/custom_encoder.cpp)
avb_add_test_binary(
    avb_video_memory_roundtrip
    integration/video_memory_roundtrip.cpp)

if(AVB_ENABLE_MEDIAFOUNDATION)
    avb_add_test_binary(
        avb_mediafoundation_runtime_codecs
        platform/mediafoundation.cpp)
    target_link_libraries(
        avb_mediafoundation_runtime_codecs PRIVATE d3d11 dxgi)
endif()

if(AVB_ENABLE_AVFOUNDATION)
    avb_add_test_binary(
        avb_platform_avfoundation
        platform/avfoundation.mm)
    set_source_files_properties(
        platform/avfoundation.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(
        avb_platform_avfoundation PRIVATE
        "-framework AVFoundation"
        "-framework CoreFoundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework Foundation"
        "-framework IOSurface"
        "-framework VideoToolbox")
endif()

if(UNIX AND NOT APPLE)
    avb_add_test_binary(
        avb_platform_linux
        platform/linux.cpp)
endif()

if(WIN32)
    target_link_libraries(avb_video_memory_roundtrip PRIVATE d3d11)
endif()

avb_add_ctest(
    api_contract avb_api_contract "contract;api")
avb_add_ctest(
    c_api_contract avb_c_api_contract "contract;api;c-abi")

find_program(FFMPEG_EXECUTABLE ffmpeg)
find_program(FFPROBE_EXECUTABLE ffprobe)

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
                -color_range tv -colorspace smpte170m
                -color_primaries smpte170m -color_trc smpte170m
                "${AVB_FIXTURE}"
        COMMENT "Generating conformance fixture with ffmpeg"
        VERBATIM
    )
    add_custom_target(avb_conformance_fixture DEPENDS "${AVB_FIXTURE}")
    set(AVB_ODD_FIXTURE "${AVB_OUT}/odd_dimensions.mp4")
    add_custom_command(
        OUTPUT "${AVB_ODD_FIXTURE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${AVB_OUT}"
        COMMAND ${FFMPEG_EXECUTABLE} -y -loglevel error
                -f lavfi -i "nullsrc=size=65x49:rate=3,format=yuv444p,geq=lum=81:cb=90:cr=240"
                -frames:v 3 -c:v libx264 -pix_fmt yuv444p
                -color_range tv -colorspace smpte170m "${AVB_ODD_FIXTURE}"
        VERBATIM)
    add_custom_target(avb_odd_fixture DEPENDS "${AVB_ODD_FIXTURE}")
    add_dependencies(avb_odd_dimensions avb_odd_fixture)
    if(TARGET avb_ffmpeg_read_errors)
        set(AVB_READ_ERROR_FIXTURE "${AVB_OUT}/read_errors.mp4")
        add_custom_command(
            OUTPUT "${AVB_READ_ERROR_FIXTURE}"
            DEPENDS "${AVB_FIXTURE}"
            COMMAND ${FFMPEG_EXECUTABLE} -y -loglevel error
                    -stream_loop 7 -i "${AVB_FIXTURE}" -c copy -movflags +faststart
                    "${AVB_READ_ERROR_FIXTURE}"
            VERBATIM)
        add_custom_target(avb_read_error_fixture DEPENDS "${AVB_READ_ERROR_FIXTURE}")
        add_dependencies(avb_read_error_fixture avb_conformance_fixture)
        add_dependencies(avb_ffmpeg_read_errors avb_read_error_fixture)
        avb_add_ctest(read_errors_ffmpeg avb_ffmpeg_read_errors
            "platform;decode;backend-ffmpeg" "${AVB_READ_ERROR_FIXTURE}"
            "${AVB_OUT}/read_errors_scratch.mp4")
    endif()
    set(AVB_AUDIO_FIXTURE "${AVB_OUT}/resample.wav")
    add_custom_command(
        OUTPUT "${AVB_AUDIO_FIXTURE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${AVB_OUT}"
        COMMAND ${FFMPEG_EXECUTABLE} -y -loglevel error
                -f lavfi -i "sine=frequency=997:duration=1:sample_rate=44100"
                -c:a pcm_s16le "${AVB_AUDIO_FIXTURE}"
        VERBATIM)
    add_custom_target(avb_audio_fixture DEPENDS "${AVB_AUDIO_FIXTURE}")
    add_dependencies(avb_audio_resample avb_audio_fixture)
    if(AVB_ENABLE_GSTREAMER)
        set(AVB_TRACK_FIXTURE "${AVB_OUT}/multitrack.mp4")
        add_custom_command(
            OUTPUT "${AVB_TRACK_FIXTURE}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${AVB_OUT}"
            COMMAND ${FFMPEG_EXECUTABLE} -y -loglevel error
                    -f lavfi -i "color=red:size=64x48:rate=10:duration=2"
                    -f lavfi -i "color=blue:size=96x64:rate=15:duration=2"
                    -f lavfi -i "sine=frequency=440:sample_rate=32000:duration=2"
                    -f lavfi -i "sine=frequency=880:sample_rate=48000:duration=2"
                    -map 0:v -map 1:v -map 2:a -map 3:a
                    -c:v:0 libx264 -c:v:1 mpeg4 -c:a aac -ac:a:1 2
                    -movflags +faststart "${AVB_TRACK_FIXTURE}"
            VERBATIM)
        add_custom_target(avb_track_fixture DEPENDS "${AVB_TRACK_FIXTURE}")
        add_dependencies(avb_stream_selection avb_track_fixture)
        avb_add_ctest(stream_selection_gstreamer avb_stream_selection
            "conformance;decode;backend-gstreamer" "${AVB_TRACK_FIXTURE}")
    endif()
    set(AVB_MF_RUNTIME_CODEC_ARGS)
    if(WIN32 AND AVB_ENABLE_MEDIAFOUNDATION)
        include(platform/mediafoundation_fixtures.cmake)
        avb_add_mediafoundation_fixtures(
            "${FFMPEG_EXECUTABLE}"
            "${AVB_OUT}"
            AVB_MF_RUNTIME_CODEC_ARGS)
    endif()

    add_dependencies(avb_conformance avb_conformance_fixture)
    add_dependencies(avb_encoder_input avb_conformance_fixture)
    add_dependencies(avb_roundtrip avb_conformance_fixture)
    add_dependencies(avb_codecs avb_conformance_fixture)
    add_dependencies(avb_io avb_conformance_fixture)
    add_dependencies(avb_custom_decoder avb_conformance_fixture)
    add_dependencies(avb_custom_encoder avb_conformance_fixture)
    if(TARGET avb_gstreamer_errors)
        add_dependencies(avb_gstreamer_errors avb_conformance_fixture)
        avb_add_ctest(read_errors_gstreamer avb_gstreamer_errors
            "platform;decode;backend-gstreamer" "${AVB_FIXTURE}")
        set_tests_properties(read_errors_gstreamer PROPERTIES TIMEOUT 20)
    endif()
    if(TARGET avb_platform_avfoundation)
        add_dependencies(
            avb_platform_avfoundation avb_conformance_fixture)
    endif()
    if(TARGET avb_platform_linux)
        add_dependencies(
            avb_platform_linux avb_conformance_fixture)
    endif()
    if(TARGET avb_mediafoundation_runtime_codecs)
        add_dependencies(
            avb_mediafoundation_runtime_codecs avb_conformance_fixture)
    endif()
    if(TARGET avb_mediafoundation_codec_fixtures AND
       TARGET avb_mediafoundation_runtime_codecs)
        add_dependencies(avb_mediafoundation_runtime_codecs
                         avb_mediafoundation_codec_fixtures)
    endif()
    add_dependencies(
        avb_video_memory_roundtrip avb_conformance_fixture)

    # Apply the same decode, encode, and I/O contract to every enabled backend.
    set(_avb_backends auto)
    set(_avb_explicit_backends)
    if(AVB_ENABLE_GSTREAMER)
        list(APPEND _avb_explicit_backends gstreamer)
    endif()
    if(AVB_ENABLE_FFMPEG)
        list(APPEND _avb_explicit_backends ffmpeg)
    endif()
    if(AVB_ENABLE_MEDIAFOUNDATION)
        list(APPEND _avb_explicit_backends mediafoundation)
    endif()
    if(AVB_ENABLE_AVFOUNDATION)
        list(APPEND _avb_explicit_backends avfoundation)
    endif()
    list(APPEND _avb_backends ${_avb_explicit_backends})

    foreach(be ${_avb_backends})
        if(be STREQUAL "ffmpeg")
            avb_add_ctest(
                audio_resample_${be} avb_audio_resample
                "conformance;decode;audio;backend-${be}"
                "${AVB_AUDIO_FIXTURE}" ${be})
        endif()
        avb_add_ctest(
            conformance_${be}
            avb_conformance
            "conformance;decode;backend-${be}"
            "${AVB_FIXTURE}" ${be})
        avb_add_ctest(
            roundtrip_${be}
            avb_roundtrip
            "integration;roundtrip;backend-${be}"
            "${AVB_FIXTURE}"
            "${AVB_OUT}/roundtrip_${be}.mp4"
            ${be})
        avb_add_ctest(
            io_${be}
            avb_io
            "conformance;io;backend-${be}"
            "${AVB_FIXTURE}" ${be})
    endforeach()

    foreach(be ${_avb_explicit_backends})
        if(be STREQUAL "ffmpeg" OR be STREQUAL "gstreamer")
            avb_add_ctest(odd_dimensions_${be} avb_odd_dimensions
                "conformance;decode;backend-${be}" "${AVB_ODD_FIXTURE}" ${be})
        endif()
        avb_add_ctest(
            encoder_input_${be} avb_encoder_input
            "conformance;encode;backend-${be}"
            "${AVB_OUT}/encoder_input_${be}.mp4" ${be})
        avb_add_ctest(
            custom_decoder_${be}
            avb_custom_decoder
            "integration;plugin;decode;backend-${be}"
            "${AVB_FIXTURE}" ${be})
        if(be STREQUAL "gstreamer")
            set(_custom_extension mkv)
        else()
            set(_custom_extension mov)
        endif()
        avb_add_ctest(
            custom_encoder_${be}
            avb_custom_encoder
            "integration;plugin;encode;backend-${be}"
            "${AVB_OUT}/custom_encoder_${be}.${_custom_extension}"
            ${be})
        if(FFPROBE_EXECUTABLE AND (be STREQUAL "ffmpeg" OR be STREQUAL "gstreamer"))
            avb_add_ctest(
                custom_encoder_timestamps_${be} ${CMAKE_COMMAND}
                "integration;plugin;encode;backend-${be}"
                "-DENCODER=$<TARGET_FILE:avb_custom_encoder>"
                "-DPROBE=${FFPROBE_EXECUTABLE}"
                "-DOUTPUT=${AVB_OUT}/custom_timestamps_${be}.${_custom_extension}"
                "-DBACKEND=${be}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/integration/custom_timestamps.cmake")
        endif()
    endforeach()

    # Wide codec coverage is backend-specific because runtime plugin sets vary.
    if(AVB_ENABLE_FFMPEG)
        avb_add_ctest(
            codecs_ffmpeg
            avb_codecs
            "integration;codec;backend-ffmpeg"
            "${AVB_FIXTURE}" "${AVB_OUT}/codec_ffmpeg" ffmpeg)
        avb_add_ctest(
            dmabuf_roundtrip_ffmpeg
            avb_video_memory_roundtrip
            "integration;hardware;dmabuf;backend-ffmpeg"
            dmabuf
            "${AVB_FIXTURE}"
            "${AVB_OUT}/dmabuf_roundtrip_ffmpeg.mp4"
            ffmpeg ffmpeg)
    endif()
    if(AVB_ENABLE_GSTREAMER)
        avb_add_ctest(
            codecs_gstreamer_video
            avb_codecs
            "integration;codec;video;backend-gstreamer"
            "${AVB_FIXTURE}"
            "${AVB_OUT}/codec_gstreamer_video"
            gstreamer video-only)
        avb_add_ctest(
            codecs_gstreamer_audio
            avb_codecs
            "integration;codec;audio;backend-gstreamer"
            "${AVB_FIXTURE}"
            "${AVB_OUT}/codec_gstreamer_audio"
            gstreamer audio-only)
        avb_add_ctest(
            dmabuf_roundtrip_gstreamer
            avb_video_memory_roundtrip
            "integration;hardware;dmabuf;backend-gstreamer"
            dmabuf
            "${AVB_FIXTURE}"
            "${AVB_OUT}/dmabuf_roundtrip_gstreamer.mp4"
            gstreamer gstreamer)
        if(AVB_ENABLE_FFMPEG)
            avb_add_ctest(
                dmabuf_roundtrip_ffmpeg_to_gstreamer
                avb_video_memory_roundtrip
                "integration;hardware;dmabuf;cross-backend"
                dmabuf
                "${AVB_FIXTURE}"
                "${AVB_OUT}/dmabuf_roundtrip_ffmpeg_to_gstreamer.mp4"
                ffmpeg gstreamer)
            avb_add_ctest(
                dmabuf_roundtrip_gstreamer_to_ffmpeg
                avb_video_memory_roundtrip
                "integration;hardware;dmabuf;cross-backend"
                dmabuf
                "${AVB_FIXTURE}"
                "${AVB_OUT}/dmabuf_roundtrip_gstreamer_to_ffmpeg.mp4"
                gstreamer ffmpeg)
        endif()
    endif()

    if(AVB_ENABLE_MEDIAFOUNDATION)
        avb_add_ctest(
            runtime_codecs_mediafoundation
            avb_mediafoundation_runtime_codecs
            "platform;codec;hardware;backend-mediafoundation"
            "${AVB_FIXTURE}"
            "${AVB_OUT}/runtime_codecs_mediafoundation_hevc.mp4"
            ${AVB_MF_RUNTIME_CODEC_ARGS})
        avb_add_ctest(
            native_roundtrip_mediafoundation
            avb_video_memory_roundtrip
            "integration;hardware;native;backend-mediafoundation"
            native
            "${AVB_FIXTURE}"
            "${AVB_OUT}/native_roundtrip_mediafoundation.mp4"
            mediafoundation mediafoundation)
    endif()

    if(AVB_ENABLE_AVFOUNDATION)
        avb_add_ctest(
            platform_avfoundation
            avb_platform_avfoundation
            "platform;hardware;native;backend-avfoundation"
            "${AVB_FIXTURE}")
        avb_add_ctest(
            native_roundtrip_avfoundation
            avb_video_memory_roundtrip
            "integration;hardware;native;backend-avfoundation"
            native
            "${AVB_FIXTURE}"
            "${AVB_OUT}/native_roundtrip_avfoundation.mp4"
            avfoundation avfoundation)
    endif()

    if(TARGET avb_platform_linux)
        avb_add_ctest(
            platform_linux
            avb_platform_linux
            "platform;hardware;dmabuf;linux"
            "${AVB_FIXTURE}")
    endif()
else()
    message(WARNING
        "ffmpeg not found; the conformance test fixture cannot be generated. "
        "Only fixture-free contract tests are enabled.")
endif()
