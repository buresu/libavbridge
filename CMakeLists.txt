cmake_minimum_required(VERSION 3.16)
project(avbridge VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(AVB_BUILD_SHARED "Build shared library" ON)
option(AVB_BUILD_EXAMPLES "Build examples" ON)
option(AVB_BUILD_TESTS "Build tests" OFF)

# Native platform backends (each only builds on its own OS).
option(AVB_ENABLE_MEDIAFOUNDATION "Enable Windows Media Foundation backend" ON)
option(AVB_ENABLE_AVFOUNDATION "Enable Apple AVFoundation backend" ON)

# GStreamer is the default backend on Linux; available (opt-in) elsewhere.
if(UNIX AND NOT APPLE)
    option(AVB_ENABLE_GSTREAMER "Enable GStreamer runtime backend" ON)
else()
    option(AVB_ENABLE_GSTREAMER "Enable GStreamer runtime backend" OFF)
endif()

# FFmpeg is an optional, cross-platform runtime backend (dynamically loaded).
# Off by default everywhere; enable it explicitly on Windows/macOS/Linux.
option(AVB_ENABLE_FFMPEG "Enable FFmpeg runtime backend" OFF)

# The native backends are OS-bound: force them off where they cannot build.
if(WIN32)
    set(AVB_ENABLE_AVFOUNDATION OFF CACHE BOOL "" FORCE)
elseif(APPLE)
    set(AVB_ENABLE_MEDIAFOUNDATION OFF CACHE BOOL "" FORCE)
elseif(UNIX AND NOT APPLE)
    set(AVB_ENABLE_MEDIAFOUNDATION OFF CACHE BOOL "" FORCE)
    set(AVB_ENABLE_AVFOUNDATION OFF CACHE BOOL "" FORCE)
endif()

set(AVB_SOURCES
    src/avb_decoder.cpp
    src/avb_error.cpp
    src/avb_backend.cpp
    src/avb_encoder.cpp
    src/avb_video_codec_registry.cpp
)

if(AVB_ENABLE_MEDIAFOUNDATION)
    list(APPEND AVB_SOURCES
        src/backends/mediafoundation/avb_backend_mediafoundation.cpp
        src/backends/mediafoundation/avb_encoder_mediafoundation.cpp
    )
endif()

if(AVB_ENABLE_AVFOUNDATION)
    list(APPEND AVB_SOURCES
        src/backends/avfoundation/avb_backend_avfoundation.mm
        src/backends/avfoundation/avb_encoder_avfoundation.mm
    )
    # The Objective-C++ backends rely on ARC for memory management of the
    # AVFoundation objects they retain across calls.
    set_source_files_properties(
        src/backends/avfoundation/avb_backend_avfoundation.mm
        src/backends/avfoundation/avb_encoder_avfoundation.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc"
    )
endif()

if(AVB_ENABLE_FFMPEG)
    list(APPEND AVB_SOURCES
        src/backends/ffmpeg/avb_backend_ffmpeg.cpp
        src/backends/ffmpeg/avb_encoder_ffmpeg.cpp
        src/backends/ffmpeg/avb_ffmpeg_loader.cpp
    )
endif()

if(AVB_ENABLE_GSTREAMER)
    list(APPEND AVB_SOURCES
        src/backends/gstreamer/avb_backend_gstreamer.cpp
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
    target_link_libraries(avbridge PRIVATE mfplat mf mfreadwrite mfuuid wmcodecdspuuid ole32)
endif()

if(AVB_ENABLE_AVFOUNDATION)
    target_compile_definitions(avbridge PRIVATE AVB_ENABLE_AVFOUNDATION)
    target_link_libraries(avbridge PRIVATE
        "-framework AVFoundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework Foundation"
    )
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

    # Link only dl for runtime loading — no FFmpeg libraries at build time.
    # Windows resolves symbols via LoadLibrary/GetProcAddress (no libdl).
    if(NOT WIN32)
        target_link_libraries(avbridge PRIVATE ${CMAKE_DL_LIBS})
    endif()
endif()

if(AVB_ENABLE_GSTREAMER)
    target_compile_definitions(avbridge PRIVATE AVB_ENABLE_GSTREAMER)

    # GStreamer/GLib headers for compilation only. The shared objects are loaded
    # at runtime via dlopen (see avb_gstreamer_loader.cpp); we never link them.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-app-1.0
                                   gstreamer-pbutils-1.0 gstreamer-video-1.0
                                   gstreamer-allocators-1.0 gobject-2.0 glib-2.0)
    target_include_directories(avbridge PRIVATE ${GST_INCLUDE_DIRS})

    # Link only dl for runtime loading — no GStreamer libraries at build time.
    target_link_libraries(avbridge PRIVATE ${CMAKE_DL_LIBS})
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
