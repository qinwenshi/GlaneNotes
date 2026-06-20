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

// Stop recording. Returns quickly: the I2S/mic are released synchronously, while
// the SD work (flush ring, write WAV header, close) finishes on a background task.
void recorder_stop(void);

// Set the unix mtime stamped on the WAV when the background finalize closes it
// (0 = leave the FAT default). Call before recorder_stop().
void recorder_set_save_time(int64_t unix_secs);

// Block until any in-flight background SD finalize has completed (or timeout).
// Call before deep sleep / powering down the SD rail to avoid a truncated file.
void recorder_wait_finalized(uint32_t timeout_ms);

bool     recorder_is_active(void);
uint32_t recorder_elapsed_ms(void);
uint32_t recorder_bytes_written(void);  // PCM data bytes (excludes 44-byte header)

#ifdef __cplusplus
}
#endif
