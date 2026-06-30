#pragma once

#include <stdbool.h>

// ElevenLabs Conversational AI client.
//
// Stage 1: opens a WebSocket to wss://api.elevenlabs.io/v1/convai/conversation
// with the xi-api-key header, sends conversation_initiation_client_data, then
// logs every event the server returns. No audio plumbing yet.
//
// Each call to convai_start() spawns a single conversation task. If a
// conversation is already running, the new call is a no-op and returns false.
// Call convai_stop() to tear it down (closes WS, frees task).
bool convai_start(const char *agent_id);
void convai_stop(void);
bool convai_is_running(void);

// Inject a text turn into the live conversation, e.g. from a custom-phrase NFC
// tag. Builds {"type":"user_message","text":"<text>"} and sends it directly over
// the WSS (bypassing the mic queue, like the other one-shot events), THEN appends
// a tail-silence pad to force the turn to end — without it the server's audio-VAD
// turn detection never sees an end-of-turn and the agent stays silent. The agent
// then treats the text as a complete user utterance and responds. No-op (returns
// false) if no conversation is running, the WS is down, or PTT is held.
bool convai_send_user_message(const char *text);

// Push-to-talk. Call with true on button press (starts streaming the mic
// to the server as user_audio_chunk events), false on release (stops the
// stream — the server's VAD then treats the gap as end-of-utterance).
// No-op if convai isn't running.
void convai_ptt_set(bool pressed);

// Toggle the noisy diagnostic logs: the per-2s heartbeat ("hb: …") and the
// per-ping "rx: type=ping" line. false = quiet (default). All other logs
// (connect/disconnect, errors, PTT, transcripts) are unaffected. Safe to
// call before convai_start(); the setting persists across start/stop.
void convai_set_log_verbose(bool verbose);
