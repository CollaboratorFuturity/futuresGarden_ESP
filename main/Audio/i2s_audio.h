#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// J9 pin map (see j9-audio-wiring.md):
//   GPIO 43 = BCLK (shared MAX98357A + INMP441 SCK)
//   GPIO 44 = LRC/WS (shared MAX98357A + INMP441 WS)
//   GPIO 19 = DIN  → MAX98357A
//   GPIO 20 = INMP441 SD (moved off GPIO 0 — strap pin contention masked
//             the mic's output; everything except loud transients read as 0)
// Plugging J9 in steals GPIO 19/20/43/44 — both USB ports go dark once
// i2s_audio_init() runs.

// Open I2S0 in duplex standard mode: 16-bit mono @ 16 kHz on both
// directions. Matches ElevenLabs `pcm_16000`.
void i2s_audio_init(void);

// Set output volume as a percentage (0–100). Applied to every TX sample
// before it's MSB-aligned into the 32-bit slot. Survives across plays.
// Defaults to 60 % until called.
void i2s_audio_set_volume_pct(int pct);

// Generate a sine tone and play it. Useful for wiring verification.
void i2s_audio_beep(int freq_hz, int duration_ms);

// Play `count` (freq_hz, duration_ms) pairs back-to-back with no audible
// gap between them. Each tone gets a short envelope so the transitions
// don't pop. One drain after the last tone.
typedef struct { int freq_hz; int duration_ms; } i2s_audio_tone_t;
void i2s_audio_tone_sequence(const i2s_audio_tone_t *tones, size_t count);

// Stream raw samples to the speaker without draining/disabling at the end.
// Use this in the future for continuous WSS audio playback.
void i2s_audio_write(const int16_t *samples, size_t count);

// One-shot playback: enable TX, write all `count` samples, drain with a
// short silence tail, disable TX. Use for buffered playback of recordings.
void i2s_audio_play(const int16_t *samples, size_t count);

// Capture `count` int16 mono samples from the mic into `out`. Bounded by
// twice the expected wall-clock duration as a safety timeout so a dead mic
// doesn't hang the caller forever. Returns true on full capture, false on
// timeout (in which case `out` holds however many samples did arrive).
bool i2s_audio_record(int16_t *out, size_t count);

// Streaming record API for push-to-talk / continuous mic.
//   record_start: open the RX channel and mute the speaker (DOUT detach).
//                 Cheap to call repeatedly — idempotent.
//   record_chunk: block until `count` mono samples are read into `out`, or
//                 a per-call timeout (250 ms) elapses. Returns the number
//                 of samples actually written.
//   record_stop:  close the RX channel and un-mute the speaker. Safe to
//                 call without start.
void   i2s_audio_record_start(void);
size_t i2s_audio_record_chunk(int16_t *out, size_t count);
void   i2s_audio_record_stop(void);
