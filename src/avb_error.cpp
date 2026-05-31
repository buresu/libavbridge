#include "avbridge/avb.h"

const char *avb_result_to_string(avb_result result) {
    switch (result) {
        case AVB_OK:                          return "OK";
        case AVB_ERROR_UNKNOWN:               return "Unknown error";
        case AVB_ERROR_INVALID_ARGUMENT:      return "Invalid argument";
        case AVB_ERROR_BACKEND_NOT_AVAILABLE: return "Backend not available";
        case AVB_ERROR_OPEN_FAILED:           return "Open failed";
        case AVB_ERROR_STREAM_NOT_FOUND:      return "Stream not found";
        case AVB_ERROR_DECODE_FAILED:         return "Decode failed";
        case AVB_ERROR_SEEK_FAILED:           return "Seek failed";
        case AVB_ERROR_EOF:                   return "End of file";
        default:                              return "Unknown error code";
    }
}
