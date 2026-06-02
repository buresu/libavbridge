cmake_minimum_required(VERSION 3.16)
project(avbridge VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(AVB_BUILD_SHARED "Build shared library" ON)
option(AVB_BUILD_EXAMPLES "Build examples" ON)
option(AVB_BUILD_TESTS "Build tests" OFF)

option(AVB_ENABLE_MEDIAFOUNDATION "Enable Windows Media Foundation backend" ON)
option(AVB_ENABLE_AVFOUNDATION "Enable Apple AVFoundation backend" ON)
option(AVB_ENABLE_FFMPEG "Enable Linux FFmpeg runtime backend" ON)

# Platform defaults
if(WIN32)
    set(AVB_ENABLE_AVFOUNDATION OFF CACHE BOOL "" FORCE)
    set(AVB_ENABLE_FFMPEG OFF CACHE BOOL "" FORCE)
elseif(APPLE)
    set(AVB_ENABLE_MEDIAFOUNDATION OFF CACHE BOOL "" FORCE)
    set(AVB_ENABLE_FFMPEG OFF CACHE BOOL "" FORCE)
elseif(UNIX AND NOT APPLE)
    set(AVB_ENABLE_MEDIAFOUNDATION OFF CACHE BOOL "" FORCE)
    set(AVB_ENABLE_AVFOUNDATION OFF CACHE BOOL "" FORCE)
endif()

set(AVB_SOURCES
    src/avb_context.cpp
    src/avb_error.cpp
    src/avb_backend.cpp
)

if(AVB_ENABLE_MEDIAFOUNDATION)
    list(APPEND AVB_SOURCES
        src/backends/mediafoundation/avb_backend_mediafoundation.cpp
    )
endif()

if(AVB_ENABLE_AVFOUNDATION)
    list(APPEND AVB_SOURCES
        src/backends/avfoundation/avb_backend_avfoundation.mm
    )
    # The Objective-C++ backend relies on ARC for memory management of the
    # AVFoundation objects it retains across calls.
    set_source_files_properties(
        src/backends/avfoundation/avb_backend_avfoundation.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc"
    )
endif()

if(AVB_ENABLE_FFMPEG)
    list(APPEND AVB_SOURCES
        src/backends/ffmpeg/avb_backend_ffmpeg.cpp
        src/backends/ffmpeg/avb_ffmpeg_loader.cpp
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

    # Find FFmpeg headers for compilation only (no link-time dependency on FFmpeg)
    find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h
        HINTS /usr/include /usr/local/include
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

    # Link only dl for runtime loading — no FFmpeg libraries at build time
    target_link_libraries(avbridge PRIVATE dl)
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

install(TARGETS avbridge
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    PUBLIC_HEADER DESTINATION include/avbridge
)
