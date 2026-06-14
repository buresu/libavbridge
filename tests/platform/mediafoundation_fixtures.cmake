function(avb_add_mediafoundation_fixtures ffmpeg output_dir out_args)
    execute_process(
        COMMAND ${ffmpeg} -hide_banner -encoders
        OUTPUT_VARIABLE ffmpeg_encoders
        ERROR_QUIET
    )

    set(audio_fixtures)
    set(video_fixtures)
    set(runtime_args)

    set(flac_fixture "${output_dir}/runtime_codecs_mediafoundation_flac.flac")
    add_custom_command(
        OUTPUT "${flac_fixture}"
        COMMAND ${ffmpeg} -y -loglevel error
                -f lavfi -i "sine=frequency=660:duration=1"
                -c:a flac -ar 44100 -ac 1
                "${flac_fixture}"
        COMMENT "Generating Media Foundation FLAC runtime fixture"
        VERBATIM
    )
    list(APPEND audio_fixtures "${flac_fixture}")
    list(APPEND runtime_args audio flac "${flac_fixture}" flac)

    if(ffmpeg_encoders MATCHES "libopus")
        set(opus_fixture
            "${output_dir}/runtime_codecs_mediafoundation_opus.ogg")
        add_custom_command(
            OUTPUT "${opus_fixture}"
            COMMAND ${ffmpeg} -y -loglevel error
                    -f lavfi -i "sine=frequency=550:duration=1"
                    -c:a libopus -ar 48000 -ac 1
                    "${opus_fixture}"
            COMMENT "Generating Media Foundation Opus runtime fixture"
            VERBATIM
        )
        list(APPEND audio_fixtures "${opus_fixture}")
        list(APPEND runtime_args audio opus "${opus_fixture}" opus)
    endif()

    if(ffmpeg_encoders MATCHES "libvorbis")
        set(vorbis_fixture
            "${output_dir}/runtime_codecs_mediafoundation_vorbis.ogg")
        add_custom_command(
            OUTPUT "${vorbis_fixture}"
            COMMAND ${ffmpeg} -y -loglevel error
                    -f lavfi -i "sine=frequency=770:duration=1"
                    -c:a libvorbis -ar 44100 -ac 1
                    "${vorbis_fixture}"
            COMMENT "Generating Media Foundation Vorbis runtime fixture"
            VERBATIM
        )
        list(APPEND audio_fixtures "${vorbis_fixture}")
        list(APPEND runtime_args audio vorbis "${vorbis_fixture}" "*")
    endif()

    if(ffmpeg_encoders MATCHES "libvpx")
        set(vp8_fixture
            "${output_dir}/runtime_codecs_mediafoundation_vp8.webm")
        add_custom_command(
            OUTPUT "${vp8_fixture}"
            COMMAND ${ffmpeg} -y -loglevel error
                    -f lavfi -i "testsrc=duration=1:size=160x120:rate=15"
                    -an -c:v libvpx -pix_fmt yuv420p -b:v 500k
                    "${vp8_fixture}"
            COMMENT "Generating Media Foundation VP8 runtime fixture"
            VERBATIM
        )
        list(APPEND video_fixtures "${vp8_fixture}")
        list(APPEND runtime_args video vp8 "${vp8_fixture}" vp8)
    endif()

    if(ffmpeg_encoders MATCHES "libvpx-vp9")
        set(vp9_fixture
            "${output_dir}/runtime_codecs_mediafoundation_vp9.webm")
        add_custom_command(
            OUTPUT "${vp9_fixture}"
            COMMAND ${ffmpeg} -y -loglevel error
                    -f lavfi -i "testsrc=duration=1:size=160x120:rate=15"
                    -an -c:v libvpx-vp9 -pix_fmt yuv420p -b:v 500k
                    "${vp9_fixture}"
            COMMENT "Generating Media Foundation VP9 runtime fixture"
            VERBATIM
        )
        list(APPEND video_fixtures "${vp9_fixture}")
        list(APPEND runtime_args video vp9 "${vp9_fixture}" vp9)
    endif()

    if(ffmpeg_encoders MATCHES "libaom-av1")
        set(av1_fixture
            "${output_dir}/runtime_codecs_mediafoundation_av1.mkv")
        add_custom_command(
            OUTPUT "${av1_fixture}"
            COMMAND ${ffmpeg} -y -loglevel error
                    -f lavfi -i "testsrc=duration=1:size=320x240:rate=15"
                    -an -c:v libaom-av1 -pix_fmt yuv420p -crf 38 -b:v 0
                    -cpu-used 8
                    "${av1_fixture}"
            COMMENT "Generating Media Foundation AV1 runtime fixture"
            VERBATIM
        )
        list(APPEND video_fixtures "${av1_fixture}")
        list(APPEND runtime_args video av1 "${av1_fixture}" av1)
    endif()

    add_custom_target(
        avb_mediafoundation_codec_fixtures
        DEPENDS ${audio_fixtures} ${video_fixtures}
    )
    set(${out_args} ${runtime_args} PARENT_SCOPE)
endfunction()
