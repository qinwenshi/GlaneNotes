// recorder.h — microphone WAV recorder
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// One-time codec/mic preparation check. Safe to call once at boot.
void recorder_init(void);

// Begin recording 16 kHz/16-bit/mono PCM to a WAV file at `vfs_path`
// (e.g. "/sdcard/notes/note-123.wav"). Returns false on failure.
bool recorder_start(const char *vfs_path);

// Stop recording, patch the WAV header, flush and close the file.
void recorder_stop(void);

bool     recorder_is_active(void);
uint32_t recorder_elapsed_ms(void);
uint32_t recorder_bytes_written(void);  // PCM data bytes (excludes 44-byte header)

#ifdef __cplusplus
}
#endif
