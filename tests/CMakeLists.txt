# Conformance test. The fixture is generated with ffmpeg at build time so the
# repository stays free of binary blobs; if ffmpeg is unavailable the test is
# skipped (registered but reported as "not run") rather than failing the build.

add_executable(avb_conformance conformance.cpp)
target_link_libraries(avb_conformance PRIVATE avbridge)
if(AVB_BUILD_SHARED)
    target_compile_definitions(avb_conformance PRIVATE AVB_USING_SHARED)
endif()

find_program(FFMPEG_EXECUTABLE ffmpeg)

if(FFMPEG_EXECUTABLE)
    set(AVB_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/fixtures/conformance.mp4")

    add_custom_command(
        OUTPUT "${AVB_FIXTURE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/fixtures"
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

    add_test(NAME conformance COMMAND avb_conformance "${AVB_FIXTURE}")
else()
    message(WARNING
        "ffmpeg not found; the conformance test fixture cannot be generated. "
        "Install ffmpeg to enable the 'conformance' test.")
endif()
