// transcribe.h — file-based speech-to-text via Aliyun DashScope (qwen3-asr-flash)
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TR_OK = 0,
    TR_NO_KEY,        // no API key configured
    TR_OPEN_FAIL,     // wav file could not be read
    TR_TOO_LARGE,     // exceeds inline upload cap
    TR_OOM,
    TR_HTTP_FAIL,     // network / HTTP error
    TR_PARSE_FAIL,    // unexpected response
} tr_result_t;

// Transcribe a 16 kHz/16-bit/mono WAV file at `wav_path` to UTF-8 text.
// On TR_OK, `out_text` holds a heap buffer the caller must free().
// Performs a single HTTPS POST (no streaming): the audio is uploaded inline
// as a base64 data URI and the recognized text is returned in the response.
tr_result_t transcribe_wav_file(const char *wav_path, char **out_text);

#ifdef __cplusplus
}
#endif
