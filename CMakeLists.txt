cmake_minimum_required(VERSION 3.16)
project(avbridge VERSION 0.5.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(AVB_BUILD_SHARED "Build shared library" ON)
option(AVB_BUILD_EXAMPLES "Build examples" ON)
option(AVB_BUILD_TESTS "Build tests" OFF)

option(AVB_ENABLE_MEDIAFOUNDATION "Enable Windows Media Foundation backend" ON)
option(AVB_ENABLE_AVFOUNDATION "Enable Apple AVFoundation backend" ON)
option(AVB_ENABLE_GSTREAMER "Enable GStreamer runtime backend" ON)
option(AVB_ENABLE_FFMPEG "Enable FFmpeg runtime backend" OFF)

if(WIN32)
    set(AVB_ENABLE_AVFOUNDATION OFF CACHE BOOL "" FORCE)
    set(AVB_ENABLE_GSTREAMER OFF CACHE BOOL "" FORCE)
elseif(APPLE)
    set(AVB_ENABLE_MEDIAFOUNDATION OFF CACHE BOOL "" FORCE)
    set(AVB_ENABLE_GSTREAMER OFF CACHE BOOL "" FORCE)
elseif(UNIX AND NOT APPLE)
    set(AVB_ENABLE_MEDIAFOUNDATION OFF CACHE BOOL "" FORCE)
    set(AVB_ENABLE_AVFOUNDATION OFF CACHE BOOL "" FORCE)
endif()

set(AVB_SOURCES
    src/avb_backend_factory.cpp
    src/avb_decoder.cpp
    src/avb_decoder_base.cpp
    src/avb_decoder_validation.cpp
    src/avb_core.cpp
    src/avb_encoder.cpp
    src/avb_encoder_validation.cpp
    src/avb_capabilities.cpp
    src/avb_probe.cpp
    src/avb_runtime_capabilities.cpp
    src/avb_video_plugins.cpp
)

if(AVB_ENABLE_MEDIAFOUNDATION)
    list(APPEND AVB_SOURCES
        src/backends/mediafoundation/avb_runtime_mediafoundation.cpp
        src/backends/mediafoundation/avb_mediafoundation_common.cpp
        src/backends/mediafoundation/avb_mediafoundation_ivf.cpp
        src/backends/mediafoundation/avb_decoder_mediafoundation.cpp
        src/backends/mediafoundation/avb_encoder_mediafoundation.cpp
    )
endif()

if(AVB_ENABLE_AVFOUNDATION)
    list(APPEND AVB_SOURCES
        src/backends/avfoundation/avb_runtime_avfoundation.cpp
        src/backends/avfoundation/avb_decoder_avfoundation.mm
        src/backends/avfoundation/avb_encoder_avfoundation.mm
    )
    set_source_files_properties(
        src/backends/avfoundation/avb_decoder_avfoundation.mm
        src/backends/avfoundation/avb_encoder_avfoundation.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc"
    )
endif()

if(AVB_ENABLE_FFMPEG)
    list(APPEND AVB_SOURCES
        src/backends/ffmpeg/avb_runtime_ffmpeg.cpp
        src/backends/ffmpeg/avb_decoder_ffmpeg.cpp
        src/backends/ffmpeg/avb_encoder_ffmpeg.cpp
        src/backends/ffmpeg/avb_ffmpeg_loader.cpp
    )
endif()

if(AVB_ENABLE_GSTREAMER)
    list(APPEND AVB_SOURCES
        src/backends/gstreamer/avb_runtime_gstreamer.cpp
        src/backends/gstreamer/avb_decoder_gstreamer.cpp
        src/backends/gstreamer/avb_encoder_gstreamer.cpp
        src/backends/gstreamer/avb_gstreamer_loader.cpp
    )
endif()

if(AVB_BUILD_SHARED)
    add_library(avbridge SHARED ${AVB_SOURCES})
    target_compile_definitions(avbridge PRIVATE AVB_BUILDING_SHARED)
else()
    add_library(avbridge STATIC ${AVB_SOURCES})
endif()

target_include_directories(avbridge
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(AVB_ENABLE_MEDIAFOUNDATION)
    target_compile_definitions(avbridge PRIVATE AVB_ENABLE_MEDIAFOUNDATION)
    target_link_libraries(avbridge PRIVATE mfplat mf mfreadwrite mfuuid wmcodecdspuuid ole32 d3d11 dxgi)
endif()

if(AVB_ENABLE_AVFOUNDATION)
    target_compile_definitions(avbridge PRIVATE AVB_ENABLE_AVFOUNDATION)
    target_link_libraries(avbridge PRIVATE
        "-framework AVFoundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework Foundation"
        "-framework VideoToolbox"
        "-framework AudioToolbox"
        "-framework CoreFoundation"
    )
endif()

if(AVB_ENABLE_GSTREAMER)
    target_compile_definitions(avbridge PRIVATE AVB_ENABLE_GSTREAMER)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-app-1.0
                                   gstreamer-pbutils-1.0 gstreamer-video-1.0
                                   gstreamer-allocators-1.0 gobject-2.0 glib-2.0)
    target_include_directories(avbridge PRIVATE ${GST_INCLUDE_DIRS})
    target_link_libraries(avbridge PRIVATE ${CMAKE_DL_LIBS})
endif()

if(AVB_ENABLE_FFMPEG)
    target_compile_definitions(avbridge PRIVATE AVB_ENABLE_FFMPEG)

    # Find FFmpeg headers for compilation only (no link-time dependency on
    # FFmpeg). Cross-platform hints; set FFMPEG_INCLUDE_DIR manually if needed.
    find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h
        HINTS /usr/include /usr/local/include
              /opt/homebrew/include            # macOS (Apple Silicon Homebrew)
              "$ENV{FFMPEG_ROOT}/include"      # Windows/manual installs
    )
    if(FFMPEG_INCLUDE_DIR)
        target_include_directories(avbridge PRIVATE ${FFMPEG_INCLUDE_DIR})
        # Verify swscale header is also present
        if(NOT EXISTS "${FFMPEG_INCLUDE_DIR}/libswscale/swscale.h")
            message(WARNING "libswscale/swscale.h not found in ${FFMPEG_INCLUDE_DIR}. "
                            "Install ffmpeg-dev (or equivalent) to enable video decode.")
        endif()
    else()
        message(WARNING "FFmpeg headers not found. FFmpeg backend may not compile correctly.")
    endif()
    if(NOT WIN32)
        target_link_libraries(avbridge PRIVATE ${CMAKE_DL_LIBS})
    endif()
endif()

set_target_properties(avbridge PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    PUBLIC_HEADER include/avbridge.h
)

if(UNIX AND NOT APPLE)
    target_compile_options(avbridge PRIVATE -fvisibility=hidden)
endif()

if(AVB_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(AVB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS avbridge
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    PUBLIC_HEADER DESTINATION include/avbridge
)
