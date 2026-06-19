// tone.h — short feedback tones (record start / stop) via ES8311 DAC.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Rising two-note cue, played just before a recording starts.
void tone_play_start(void);

// Falling two-note cue, played just after a recording stops.
void tone_play_stop(void);

// Single short beep (frequency in Hz, duration in ms). Blocking.
void tone_beep(int freq_hz, int duration_ms);

#ifdef __cplusplus
}
#endif
