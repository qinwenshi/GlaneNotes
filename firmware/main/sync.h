// sync.h — note scanning & batch transcription
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_NOTES 256

typedef struct {
    char     id[40];       // base name without extension, e.g. "note-0000000042"
    uint32_t wav_bytes;
    bool     has_text;
    time_t   mtime;        // WAV file modification time (FAT), 0/epoch if unset
} note_info_t;

// Ensure the notes directory exists.
void notes_ensure_dir(void);

// Scan NOTES_DIR for *.wav, filling `out` (up to MAX_NOTES). Returns count.
// Sorted by id descending (newest first when ids are timestamps/counters).
int notes_scan(note_info_t *out, int max);

// Count of notes whose transcript (.txt) is missing.
int notes_pending_count(void);

// Count of notes still needing any sync work (missing transcript, or — when an
// inbox webhook is configured — not yet delivered to it).
int sync_pending_count(void);

// Read a note's transcript into a freshly malloc'd buffer (caller frees), or
// NULL if none. Used by the web dashboard.
char *notes_read_text(const char *id);

// Progress callback during sync (done, total).
typedef void (*sync_progress_cb_t)(int done, int total);

// Transcribe every note missing a .txt. Returns number successfully written.
// Requires Wi-Fi connected and an API key set (caller's responsibility).
int sync_run(sync_progress_cb_t cb);

#ifdef __cplusplus
}
#endif
