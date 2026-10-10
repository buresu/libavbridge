# Inspect muxed timestamps independently of avbridge's decoder.
foreach(mode ticks stream_ticks flush_ticks seconds fallback frame_fallback flush_error)
    execute_process(COMMAND "${ENCODER}" "${OUTPUT}" "${BACKEND}" "${mode}"
        RESULT_VARIABLE result OUTPUT_VARIABLE log ERROR_VARIABLE errors)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${mode}: encoder failed: ${log}\n${errors}")
    endif()
    if(mode STREQUAL "flush_error")
        continue()
    endif()
    execute_process(COMMAND "${PROBE}" -v quiet -select_streams v:0
        -show_entries packet=pts_time,dts_time,duration_time -of csv=p=0 "${OUTPUT}"
        RESULT_VARIABLE result OUTPUT_VARIABLE timestamps)
    string(STRIP "${timestamps}" timestamps)
    if(NOT result EQUAL 0 OR NOT timestamps STREQUAL "0.000000,0.000000,0.500000\n0.500000,0.500000,0.500000\n1.000000,1.000000,0.500000\n1.500000,1.500000,0.500000")
        message(SEND_ERROR "${mode}: incorrect muxed PTS/DTS/duration: ${timestamps}")
    endif()
endforeach()
file(REMOVE "${OUTPUT}")
