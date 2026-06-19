// player.h — on-device WAV playback through the ES8311 DAC + speaker.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// One-time setup (creates the done semaphore). Safe to call once at boot.
void player_init(void);

// Start asynchronous playback of a 16-bit PCM WAV file. Returns false if the
// file can't be opened/parsed or playback is already running. Plays in its own
// task; poll player_is_active() to know when it finishes, or call player_stop().
bool player_play(const char *wav_path);

// Request stop and block until the playback task has fully torn down audio.
void player_stop(void);

bool player_is_active(void);

#ifdef __cplusplus
}
#endif
