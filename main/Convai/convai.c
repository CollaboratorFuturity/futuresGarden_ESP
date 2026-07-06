#include "convai.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/ringbuf.h"
#include "freertos/idf_additions.h"   // xTaskCreatePinnedToCoreWithCaps
#include "cJSON.h"
#include "mbedtls/base64.h"

#include "secrets.h"
#include "i2s_audio.h"
#include "orb_ui.h"
#include "LVGL_Driver.h"
#include "ota.h"

// GTS Root R1 — embedded at link time; anchors api.elevenlabs.io's chain
// (api.elevenlabs.io → GTS WR3 → GTS Root R1). The built-in mbedtls CA
// bundle failed to match this root on our build, so pin it explicitly.
extern const char gts_root_r1_pem_start[] asm("_binary_gts_root_r1_pem_start");
extern const char gts_root_r1_pem_end[]   asm("_binary_gts_root_r1_pem_end");

#define TAG "convai"

// inactivity_timeout extends the server's "no client traffic" cutoff from
// the 20 s default up to the 180 s max. Per the ElevenLabs docs + Python
// SDK source, the *only* required keepalive is replying to server `ping`
// events with `pong` (handled in process_message). There is no need for a
// periodic client-side `user_activity` — that event exists but is meant
// for app-triggered "user is interacting" hints, not a timer.
#define WS_URI_FMT             "wss://api.elevenlabs.io/v1/convai/conversation?agent_id=%s&inactivity_timeout=120"
#define WS_NETWORK_TIMEOUT_MS  10000
#define WS_RECONNECT_DELAY_MS  1500   // was 5000; shorter so any disconnect (genuine WiFi/ECONNRESET) recovers in ~2 s, not ~8 s
#define WS_BUFFER_SIZE         8192

// Reassembly buffer for fragmented WS messages. The pre-generated greeting
// arrives as a single ~250 KB audio event; live agent turns come in
// smaller chunks (per the ElevenLabs Python SDK there's no upper bound, so
// long monologues could in theory exceed this). 2 MB covers ~30 s of single-
// shot audio — well past any expected greeting; if longer single-event turns
// happen in practice we'll see overflow warnings and bump this.
#define REASM_SIZE             (2 * 1024 * 1024)

// PCM ring between WS receive and the speaker. Sized to hold ~a whole turn
// (512 KB ≈ 16 s at the 32 KB/s speaker rate), deliberately: the WS task
// chunk-decodes base64 and pushes here with portMAX_DELAY backpressure, so if
// the ring is too small the WS task BLOCKS mid-turn, stops draining the socket,
// and the WiFi RX pool exhausts → `wifi:m f null` frame drops + retransmit lag.
// A big PSRAM ring lets the WS task dump the turn and get straight back to
// reading. No latency cost (playback starts on the first byte) and no extra
// screen bandwidth (drains at a fixed 32 KB/s regardless of size). If very long
// monologues still log `m f null`, bump to 1 MB — or do the decode-decouple
// refactor (WS task hands raw audio to a decode task; see CONVAI.md / plan).
// Was 128 KB (4 s), which back-pressured on any turn longer than that.
#define PCM_RING_BYTES         (512 * 1024)

// Decoded-audio chunk written to the speaker per i2s_channel_write. Bigger
// = less overhead; smaller = lower latency / better responsiveness when we
// want to drop the queue on interrupt. 16 kHz × 64 ms = 1024 samples.
#define PLAYBACK_CHUNK_SAMPLES 1024

static esp_websocket_client_handle_t s_ws = NULL;
static bool                 s_running   = false;
static char                 s_agent_id[64];

// PTT state — set from button task, read by mic_task.
static volatile bool        s_ptt_held  = false;
static TaskHandle_t         s_mic_task  = NULL;

// Reassembly state (only touched from the websocket client's task).
static uint8_t             *s_reasm     = NULL;   // PSRAM
static int                  s_reasm_len = 0;
static uint8_t              s_reasm_op  = 0;

// Decoded PCM stream (producer = WS handler, consumer = playback task).
static StreamBufferHandle_t s_pcm       = NULL;
static uint8_t             *s_pcm_storage    = NULL;
static StaticStreamBuffer_t s_pcm_struct;

static TaskHandle_t         s_playback_task  = NULL;
static TaskHandle_t         s_heartbeat_task = NULL;
static TaskHandle_t         s_sender_task    = NULL;

// App-level send queue (replaces the direct fire-from-mic_task pattern that
// drove esp_websocket_client into fail-fast disconnects on any transient
// upstream congestion). mic_task enqueues; sender_task pops + sends with
// backoff-on-failure. The lib only ever sees successful sends with gaps,
// which doesn't trip its "transport is dead" heuristic.
//
// Storage lives in PSRAM (1 MB ≈ 24 s of mic audio at the actual 43 KB/s
// upstream rate). Per user requirement, NEVER drop frames — producer
// blocks (portMAX_DELAY) on overflow rather than dropping. Mic capture
// will run slightly behind realtime during sustained outages.
#define SEND_QUEUE_BYTES         (1 * 1024 * 1024)
#define SEND_RETRY_BACKOFF_MS    500
// Timeout passed to esp_websocket_client_send_text — it propagates to the
// transport's poll_write. At 500 ms it was a hair-trigger: on the FIRST user
// turn (right after the ~200-330 KB greeting download, on a fresh TCP conn
// still in slow-start), the uplink can't absorb the 43 KB/s mic burst within
// 500 ms → transport_poll_write returns 0 (timeout, NOT a real error:
// transport_error=ESP_OK, esp_tls_err/sock_errno uninitialized) → the WS lib
// treats the zero-length write as a dead transport and DISCONNECTS + re-greets.
// Observed ~65% of boots, sometimes 5 reconnects before a turn stuck. The
// send queue can't prevent it — the disconnect happens INSIDE the send call,
// before our retry logic runs. Raising the timeout lets sender_task ride out
// the transient congestion instead; the 1 MB PSRAM queue absorbs the mic
// backlog while a send blocks (q_depth grows, no frames dropped), and a longer
// timeout only bites when actually congested. Kept under WS_NETWORK_TIMEOUT_MS
// (10 s) so a genuinely dead transport is still caught by the lib's own timeout.
#define SEND_TEXT_TIMEOUT_MS     5000
static uint8_t            *s_queue_storage = NULL;
static StaticRingbuffer_t  s_queue_struct;
static RingbufHandle_t     s_queue = NULL;
static volatile uint32_t   s_queue_block_count = 0;   // producer had to wait
static volatile uint32_t   s_send_retries      = 0;   // failed-send retry attempts
// Track depth manually — xRingbufferGetCurFreeSize for RINGBUF_TYPE_NOSPLIT
// returns "max item that fits right now", which is roughly half the buffer
// when empty (because no-split items can't wrap). So depth = bytes_in -
// bytes_out, computed by the producer + sender themselves.
static volatile uint32_t   s_q_bytes_in        = 0;
static volatile uint32_t   s_q_bytes_out       = 0;

// End-of-turn detection for the playback path.
// ElevenLabs sends `agent_response` with the LAST audio chunk of a turn —
// that's the authoritative "no more audio coming" signal. Until we see it,
// any empty-ring period is just a mid-turn TTS gap (very common on slow
// connections, plus the first-turn LLM/TTS cold start) and the UI must
// stay on ORB_AGENT.
//
// Safety fallback: if we go this many ms without any new audio bytes
// arriving AND the ring is empty AND we never saw `agent_response`, force
// MUTED — protects against a dead server / silently-closed session leaving
// the orb pinned on ORB_AGENT forever. Measured in network arrivals (not
// playback drains) so a slow-but-real long answer with multi-second
// inter-chunk gaps still counts as "alive" as long as bytes keep arriving.
#define PLAYBACK_NO_AUDIO_SAFETY_MS  10000
static volatile bool    s_agent_done_signal  = false;
static volatile int64_t s_last_audio_rx_us   = 0;

// True while playback_task is actively playing agent audio (i.e. orb is in
// ORB_AGENT). mic_task reads this to gate PTT: presses during agent speech
// are ignored — the user must wait for the agent to finish. No more
// interrupting agent replies.
static volatile bool    s_agent_speaking     = false;

// Set the moment the agent's *audio* response starts arriving (in
// handle_audio_event). Used only to abort the NFC-inject turn-end pad early:
// once the agent is responding, the server has already closed the turn, so the
// remaining silence frames are pointless AND harmful — they'd fight the WS-lib
// recursive lock that handle_audio_event holds while draining audio, which is
// what caused the post-inject WS disconnect. Cleared by convai_send_user_message
// right before it sends the (abortable) pad. Not consulted by the PTT tail.
static volatile bool    s_turn_response_started = false;

// Observability — used by ws_event_handler error path + heartbeat_task.
// All updates are write-once-from-single-task so plain assignment is fine.
static volatile int64_t  s_last_rx_us      = 0;   // last byte received from WS
static volatile int64_t  s_last_tx_us      = 0;   // last successful client send
static volatile uint32_t s_rx_bytes_total  = 0;
static volatile uint32_t s_tx_bytes_total  = 0;
static char              s_last_msg_type[32] = {0};
static volatile uint32_t s_ws_connect_count    = 0;
static volatile uint32_t s_ws_disconnect_count = 0;

// Verbosity gate for the per-2s heartbeat and the per-ping "rx: type=ping"
// line. Default quiet; flip via convai_set_log_verbose() (wired to
// ORB_VERBOSE_CONVAI_LOGS in main.c).
static volatile bool     s_log_verbose = false;

// Ping/pong reliability. A healthy session has pongs == pings — every server
// ping triggers a pong reply inline in process_message. A divergence means
// a pong send failed (most likely the WS lock was held by the receiving task
// during a slow software-AES decrypt) → server will eventually time us out.
// Pongs are deliberately skipped while PTT is held: the mic_task is busy
// sending audio frames every 30 ms and we don't want the pong reply to
// contend for the same WS lock (per the legacy Pi codebase, which cancels
// its pong task during user audio for exactly this reason).
static volatile uint32_t s_pings_received   = 0;
static volatile uint32_t s_pongs_sent       = 0;
static volatile uint32_t s_pong_send_failed = 0;
static volatile uint32_t s_pongs_skipped_ptt = 0;

// PTT-close timestamp for the "stale turn" watchdog. Set by mic_task at the
// PTT-release edge (real-turn branch), cleared by process_message when any
// non-ping/pong server event arrives. Heartbeat drives a two-stage recovery
// off this timestamp: WARN+nudge at PTT_STALE_TURN_WARN_MS, then give-up at
// PTT_STALE_TURN_RECOVER_MS. All three are reset together (PTT-close edge and
// on any real server event) so each turn starts clean.
static volatile int64_t  s_ptt_close_us     = 0;
static volatile bool     s_stale_warned     = false;   // stage-1 one-shot
static volatile bool     s_stale_recovered  = false;   // stage-2 one-shot
// Set by heartbeat_task's stale watchdog, consumed by mic_task (the owner of
// the s_mic_* buffers) when the mic is idle — re-sends the tail silence pad to
// nudge the server's turn-detection VAD. Routed through mic_task, NOT sent from
// heartbeat_task directly, so send_silence_frames never races a live capture on
// s_mic_pcm.
static volatile bool     s_pad_resend_request = false;

// ─── Mic streaming task (PTT) ───────────────────────────────────────────
// Implements PTT per BEHAVIOR.md §3. Frame cadence is 30 ms (480 samples
// at 16 kHz, 960 bytes) — finer than the Python SDK's 250 ms because the
// Pi build uses 30 ms and we want byte-identical server-side framing.

#define MIC_CHUNK_SAMPLES        480                       // 30 ms @ 16 kHz
#define MIC_CHUNK_BYTES          (MIC_CHUNK_SAMPLES * sizeof(int16_t))   // 960
#define MIC_B64_CAP              ((MIC_CHUNK_BYTES * 4) / 3 + 4)         // 1284
#define MIC_JSON_CAP             (MIC_B64_CAP + 64)                      // wrapper

// PTT timing — based on BEHAVIOR.md §3.1. Tail pad must exceed the server's
// turn-detection silence threshold (we explicitly pin that to 800 ms in
// send_initiation), with margin for jitter. 43 × 30 = 1290 ms ≈ 500 ms over.
// Was 27 frames (810 ms); merged turns at that value because we landed right
// on the server threshold. Do not shorten without lowering the server VAD
// threshold to match.
#define PTT_POWER_RAIL_WAIT_MS   150         // §3.3 step 2
#define PTT_PRESS_MIN_MS         1000        // < this → silent revert (§3.4)
#define PTT_TAIL_SILENCE_FRAMES  43          // 43 × 30 ms ≈ 1290 ms pad
#define PTT_SHORT_TURN_MIN_MS    800         // §2.4 short-turn skip threshold

// If the server doesn't send any non-ping message within this window after
// PTT close (real-audio branch only), heartbeat WARNs and asks mic_task to
// re-send the tail pad (stage 1 — nudge the server VAD). First-turn LLM cold
// start can legitimately take ~10s; raised from 5000 after we saw false alarms
// on slow but valid first responses. The nudge is harmless if the turn is
// merely slow (the server has already closed the user turn; extra trailing
// silence is ignored).
#define PTT_STALE_TURN_WARN_MS   8000
// If STILL nothing this long after PTT close, give up on the turn (stage 2):
// revert the UI to ORB_MUTED and clear the watchdog so the user gets a
// responsive orb back instead of a permanently-stuck ORB_LOADING. Comfortably
// past the ~10s legitimate cold-start max so a slow-but-valid turn is never
// aborted (a real response clears s_ptt_close_us long before this fires).
#define PTT_STALE_TURN_RECOVER_MS  16000

static int16_t *s_mic_pcm  = NULL;
static char    *s_mic_b64  = NULL;
static char    *s_mic_json = NULL;

// Push a JSON payload into the send queue. Non-blocking happy path; falls
// back to a blocking send (portMAX_DELAY) if the queue is full so we never
// drop user audio. The block-count counter surfaces in the heartbeat log so
// we can see how often the queue gets stressed.
static int convai_send_enqueue(const char *json, size_t len)
{
    if (!s_queue) return -1;
    if (xRingbufferSend(s_queue, json, len, 0) == pdTRUE) {
        s_q_bytes_in += len;
        return (int)len;
    }
    s_queue_block_count++;
    // Producer blocks until sender drains. With 1 MB of capacity this only
    // hits during a multi-second outage — well past any normal hiccup.
    if (xRingbufferSend(s_queue, json, len, portMAX_DELAY) == pdTRUE) {
        s_q_bytes_in += len;
        return (int)len;
    }
    return -1;   // can only happen if s_running was flipped under us
}

// Sender task — drains the send queue and pushes items to the WS. On send
// failure, holds the item locally and retries every SEND_RETRY_BACKOFF_MS
// without ever returning it to the lib until success (or convai_stop).
// This is the entire point of the queue: the WS lib never sees a fail-fast
// pattern of sends, so its "transport is dead" heuristic never fires on
// transient upstream congestion.
static void sender_task(void *arg)
{
    ESP_LOGI(TAG, "sender_task started (queue=%p, ws=%p)", s_queue, s_ws);
    uint32_t sent_count = 0;
    uint32_t fail_streak = 0;
    char local[MIC_JSON_CAP];
    while (s_running) {
        if (!s_queue) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        size_t item_sz = 0;
        char *item = (char *)xRingbufferReceive(s_queue, &item_sz, pdMS_TO_TICKS(500));
        if (!item) continue;

        size_t local_sz = item_sz;
        if (local_sz > sizeof(local)) local_sz = sizeof(local);
        memcpy(local, item, local_sz);
        vRingbufferReturnItem(s_queue, item);

        while (s_running) {
            if (!s_ws) {
                vTaskDelay(pdMS_TO_TICKS(SEND_RETRY_BACKOFF_MS));
                continue;
            }
            int sent = esp_websocket_client_send_text(s_ws, local, local_sz,
                                                     pdMS_TO_TICKS(SEND_TEXT_TIMEOUT_MS));
            if (sent > 0) {
                s_last_tx_us = esp_timer_get_time();
                s_tx_bytes_total += sent;
                s_q_bytes_out += local_sz;
                sent_count++;
                // Every 33 frames (~1 s of mic) confirm sender is alive +
                // making progress, plus surface any prior fail streak that
                // recovered.
                if (sent_count % 33 == 0) {
                    ESP_LOGI(TAG, "sender: sent #%u (item=%uB, last_fail_streak=%u)",
                             (unsigned)sent_count, (unsigned)local_sz, (unsigned)fail_streak);
                }
                fail_streak = 0;
                break;
            }
            // Single-line warn on first failure of a streak; counter
            // captures continuing failures.
            if (fail_streak == 0) {
                ESP_LOGW(TAG, "sender: send_text returned %d (item=%uB, ws=%p, "
                              "connected=%d) — entering retry loop",
                         sent, (unsigned)local_sz, s_ws,
                         s_ws ? esp_websocket_client_is_connected(s_ws) : 0);
            }
            fail_streak++;
            s_send_retries++;
            vTaskDelay(pdMS_TO_TICKS(SEND_RETRY_BACKOFF_MS));
        }
    }
    ESP_LOGI(TAG, "sender_task exiting (sent %u total)", (unsigned)sent_count);
    vTaskDelete(NULL);
}

// Encode a mic frame to base64+JSON and enqueue it for the sender. Returns
// the enqueued length on success, or -1 on encoding error. Pre-queue, this
// function performed the actual WS write; now it always returns immediately
// after enqueue (the queue absorbs network jitter).
static int mic_send_chunk_with_status(size_t samples)
{
    if (samples == 0) return 0;
    size_t b64_len = 0;
    int rc = mbedtls_base64_encode((uint8_t *)s_mic_b64, MIC_B64_CAP, &b64_len,
                                   (const uint8_t *)s_mic_pcm, samples * sizeof(int16_t));
    if (rc != 0) {
        ESP_LOGE(TAG, "mic: b64 encode rc=%d", rc);
        return -1;
    }
    int json_len = snprintf(s_mic_json, MIC_JSON_CAP,
                            "{\"user_audio_chunk\":\"%.*s\"}",
                            (int)b64_len, s_mic_b64);
    if (json_len <= 0 || json_len >= (int)MIC_JSON_CAP) {
        ESP_LOGE(TAG, "mic: json snprintf overflow (%d)", json_len);
        return -1;
    }
    return convai_send_enqueue(s_mic_json, json_len);
}

// Send N silence frames (each 30 ms) back-to-back, paced at one frame per
// 30 ms so the server sees real-time cadence. Used as the post-PTT tail
// pad (BEHAVIOR.md §3.4). Buffer is assumed pre-zeroed.
//
// abortable: if true, stop early the moment the agent starts responding
// (s_turn_response_started). Only the NFC-inject pad sets this — once the agent
// replies, the server has already closed the turn, so the rest of the pad is
// wasted and would contend with the WS-lib lock held by handle_audio_event.
// The PTT tail passes false: it must always fully send (and the flag is
// stale-true from the previous turn).
static void send_silence_frames(int n, bool abortable)
{
    memset(s_mic_pcm, 0, MIC_CHUNK_BYTES);
    for (int i = 0; i < n && s_running; i++) {
        if (abortable && s_turn_response_started) break;
        mic_send_chunk_with_status(MIC_CHUNK_SAMPLES);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void mic_task(void *arg)
{
    bool       open = false;
    int64_t    press_start_us = 0;
    int        chunks_this_press = 0;

    while (s_running) {
        bool ptt = s_ptt_held;

        // ── Stale-turn nudge (stage 1): heartbeat_task asks us to re-send the
        // tail pad when the server missed the end-of-turn. Done here, on the
        // mic buffers' owning task and only while the mic is idle, so
        // send_silence_frames never races a live capture on s_mic_pcm. ───────
        if (s_pad_resend_request && !open && !ptt) {
            s_pad_resend_request = false;
            ESP_LOGW(TAG, "stale-turn nudge: re-sending %d-frame tail pad",
                     PTT_TAIL_SILENCE_FRAMES);
            send_silence_frames(PTT_TAIL_SILENCE_FRAMES, false);
        }

        // ── Rising edge: ptt=true while mic closed ─────────────────────
        if (ptt && !open) {
            // PTT is disabled while the agent is speaking — user must wait
            // for the agent to finish. Swallow the press: wait until the
            // user releases, then go back to idle so the next press is
            // evaluated cleanly. (No half-press latching, no interruption
            // event sent to the server.)
            if (s_agent_speaking) {
                ESP_LOGI(TAG, "PTT ignored — agent is speaking");
                while (s_ptt_held && s_running) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
                continue;
            }

            // 150 ms power-rail stabilization (BEHAVIOR.md §3.3 step 2).
            // If the user releases during the wait, the press was much too
            // short — bail out without opening the mic at all.
            vTaskDelay(pdMS_TO_TICKS(PTT_POWER_RAIL_WAIT_MS));
            if (!s_ptt_held) {
                ESP_LOGI(TAG, "PTT: aborted in stabilization window (<%d ms)",
                         PTT_POWER_RAIL_WAIT_MS);
                continue;
            }
            // TODO: cancel keepalive task here when it lands (§5.3).
            press_start_us = esp_timer_get_time();
            i2s_audio_record_start();
            open = true;
            chunks_this_press = 0;
            orb_ui_set_state(ORB_USER_TALK);   // green: mic hot
            ESP_LOGI(TAG, "PTT: mic open");
        }

        // ── Falling edge: ptt=false while mic open ─────────────────────
        if (!ptt && open) {
            i2s_audio_record_stop();
            open = false;
            int duration_ms      = (int)((esp_timer_get_time() - press_start_us) / 1000);
            int real_audio_ms    = chunks_this_press * 30;        // §2.4 metric

            if (duration_ms < PTT_PRESS_MIN_MS) {
                // §3.4 silent revert: press too short. No pad, no turn end.
                ESP_LOGI(TAG, "PTT: silent revert (%d ms < %d ms, %d frames sent)",
                         duration_ms, PTT_PRESS_MIN_MS, chunks_this_press);
                orb_ui_set_state(ORB_MUTED);
            } else if (real_audio_ms < PTT_SHORT_TURN_MIN_MS) {
                // §2.4 short-turn skip: press was long enough but real audio
                // was sparse. We still send the silence pad so the server
                // closes its turn cleanly (otherwise the next PTT press
                // would be appended → merged transcript), but we *don't*
                // wait for a response: no ORB_LOADING, no stale-turn
                // watchdog — straight back to ORB_MUTED.
                ESP_LOGI(TAG, "PTT: short-turn skip (real audio %d ms < %d ms, press %d ms) — "
                              "padding to close turn, not waiting for response",
                         real_audio_ms, PTT_SHORT_TURN_MIN_MS, duration_ms);
                orb_ui_set_state(ORB_MUTED);
                send_silence_frames(PTT_TAIL_SILENCE_FRAMES, false);
            } else {
                ESP_LOGI(TAG, "PTT: closed (%d ms held, real %d ms in %d frames) — pad %d ms",
                         duration_ms, real_audio_ms, chunks_this_press,
                         PTT_TAIL_SILENCE_FRAMES * 30);
                orb_ui_set_state(ORB_LOADING);   // yellow: waiting for agent
                s_ptt_close_us = esp_timer_get_time();
                s_stale_warned = false;
                s_stale_recovered = false;
                s_pad_resend_request = false;
                send_silence_frames(PTT_TAIL_SILENCE_FRAMES, false);
            }
            continue;
        }

        // ── Streaming: open mic, ship a 30 ms frame ────────────────────
        if (open) {
            size_t got = i2s_audio_record_chunk(s_mic_pcm, MIC_CHUNK_SAMPLES);
            if (got == 0) {
                // RX timeout — pad with silence so cadence stays steady.
                memset(s_mic_pcm, 0, MIC_CHUNK_BYTES);
                got = MIC_CHUNK_SAMPLES;
            } else if (got < MIC_CHUNK_SAMPLES) {
                memset(s_mic_pcm + got, 0, (MIC_CHUNK_SAMPLES - got) * sizeof(int16_t));
                got = MIC_CHUNK_SAMPLES;
            }
            mic_send_chunk_with_status(got);
            chunks_this_press++;

            // Log first 2 chunks for confirmation, then every ~1 s thereafter
            // (33 frames). Avoids flooding logs at 33 chunks/sec.
            if (chunks_this_press <= 2 || (chunks_this_press % 33) == 0) {
                int32_t peak = 0;
                int64_t sum_abs = 0;
                for (size_t i = 0; i < got; i++) {
                    int s = s_mic_pcm[i];
                    if (s < 0) s = -s;
                    if (s > peak) peak = s;
                    sum_abs += s;
                }
                ESP_LOGI(TAG, "mic frame #%d: peak=%ld mean=%ld",
                         chunks_this_press, (long)peak,
                         (long)(sum_abs / (int64_t)got));
            }
        } else {
            // Idle — no PTT, no tail. Cheap wait; latency to next press
            // dominated by 150 ms stabilization anyway.
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    if (open) i2s_audio_record_stop();
    vTaskDelete(NULL);
}

void convai_ptt_set(bool pressed)
{
    if (!s_running) return;
    s_ptt_held = pressed;
}

// ─── Audio playback task ────────────────────────────────────────────────

// Drains the PCM ring buffer and writes to I2S. Blocks on the ring; pacing
// is naturally enforced by i2s_audio_write's internal DMA wait.
//
// End-of-turn detection is protocol-based, not timer-based:
//   • Stay on ORB_AGENT as long as audio is playing.
//   • When the ring goes empty, only transition to ORB_MUTED if EITHER
//     `agent_response` arrived (ElevenLabs's authoritative end-of-turn
//     signal) OR no audio bytes have arrived in
//     PLAYBACK_NO_AUDIO_SAFETY_MS (server died / silent close).
//   • Mid-turn TTS gaps (very common on slow internet) keep the UI on
//     ORB_AGENT — audio may stutter audibly, but the UI doesn't flicker.
static void playback_task(void *arg)
{
    int16_t chunk[PLAYBACK_CHUNK_SAMPLES];
    bool was_playing = false;
    while (1) {
        size_t got = xStreamBufferReceive(s_pcm, chunk, sizeof(chunk), pdMS_TO_TICKS(250));
        if (got > 0) {
            if (!was_playing) {
                orb_ui_set_state(ORB_AGENT);  // blue: agent speaking
                was_playing = true;
                s_agent_speaking = true;      // mic_task will refuse PTT now
            }
            i2s_audio_write(chunk, got / sizeof(int16_t));
            continue;
        }

        // got == 0 → ring drained or no data within 250 ms timeout.
        if (!was_playing) continue;     // nothing playing → nothing to do

        bool done_signal = s_agent_done_signal;
        int64_t since_audio_ms = 0;
        if (s_last_audio_rx_us > 0) {
            since_audio_ms = (esp_timer_get_time() - s_last_audio_rx_us) / 1000;
        }
        bool safety_fired = since_audio_ms >= PLAYBACK_NO_AUDIO_SAFETY_MS;

        if (!done_signal && !safety_fired) {
            // Mid-turn TTS gap — stay on ORB_AGENT, just keep polling.
            continue;
        }

        if (safety_fired && !done_signal) {
            ESP_LOGW(TAG, "playback: no audio rx for %lldms and no agent_response — "
                          "forcing MUTED (server likely dead)", (long long)since_audio_ms);
        }
        was_playing = false;
        s_agent_speaking = false;        // PTT is allowed again
        s_agent_done_signal = false;
        orb_ui_set_state(ORB_MUTED);
    }
}

// ─── Event handling ──────────────────────────────────────────────────────

static void handle_audio_event(const cJSON *audio_evt)
{
    const cJSON *b64 = cJSON_GetObjectItemCaseSensitive(audio_evt, "audio_base_64");
    if (!cJSON_IsString(b64) || !b64->valuestring) return;

    // Stamp on every audio arrival so the playback-task safety fallback
    // resets — gaps within a turn keep the timer fresh as long as bytes
    // keep flowing on the wire.
    s_last_audio_rx_us = esp_timer_get_time();

    // The agent is now responding — signal the inject pad to stop (see
    // s_turn_response_started). This is also the exact point the WS task begins
    // holding client->lock in the blocking xStreamBufferSend below, which is
    // what a still-running pad would contend with.
    s_turn_response_started = true;

    const char *b64_str = b64->valuestring;
    size_t b64_total = strlen(b64_str);

    // Chunk-decode the base64 in 1 KB groups (must be a multiple of 4 chars
    // so we don't split a base64 quad) → 768 bytes of PCM per chunk. Small
    // on purpose: this runs on the websocket_client task whose stack is
    // ~4 KB and is already shared with cJSON + mbedTLS. Push each chunk to
    // the ring with portMAX_DELAY backpressure. The 512 KB ring holds ~a whole
    // turn so this normally does NOT block; if a turn exceeds it, backpressure
    // paces the WS task to the speaker rate (correct, but that blocking is what
    // starves WiFi RX → `wifi:m f null`, which is why the ring is big — see
    // CLAUDE.md "Memory" and the deferred decode-decouple refactor in CONVAI.md).
    enum { B64_CHUNK = 1024, PCM_CHUNK = (B64_CHUNK / 4) * 3 };
    uint8_t pcm[PCM_CHUNK];
    size_t consumed = 0;
    size_t total_pcm = 0;

    while (consumed < b64_total) {
        size_t take = b64_total - consumed;
        if (take > B64_CHUNK) take = B64_CHUNK;

        size_t pcm_len = 0;
        int rc = mbedtls_base64_decode(pcm, sizeof(pcm), &pcm_len,
                                       (const uint8_t *)(b64_str + consumed), take);
        if (rc != 0) {
            ESP_LOGE(TAG, "audio: base64 rc=%d at offset %u (take=%u)",
                     rc, (unsigned)consumed, (unsigned)take);
            return;
        }
        // Blocking push — only blocks if the 512 KB ring fills (a turn longer
        // than ~16 s); then we wait for the speaker to drain. Survives
        // arbitrarily long turns; the size keeps it from blocking on normal ones.
        xStreamBufferSend(s_pcm, pcm, pcm_len, portMAX_DELAY);

        consumed += take;
        total_pcm += pcm_len;
    }
    ESP_LOGI(TAG, "audio: b64=%u pcm=%u (≈ %u ms)",
             (unsigned)b64_total, (unsigned)total_pcm,
             (unsigned)(total_pcm * 1000 / (16000 * 2)));
}

static void process_message(const char *data, int len)
{
    if (len <= 0 || !data) return;

    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        ESP_LOGW(TAG, "msg: not valid JSON (%d bytes): %.*s", len,
                 len < 120 ? len : 120, data);
        return;
    }

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const char *type_str = (cJSON_IsString(type) && type->valuestring) ? type->valuestring : "?";

    // Record every type+size — invaluable for figuring out which event a hang
    // followed (silent server close, missed pong, agent_response_correction,
    // client_tool_call, etc).
    strncpy(s_last_msg_type, type_str, sizeof(s_last_msg_type) - 1);
    s_last_msg_type[sizeof(s_last_msg_type) - 1] = '\0';
    // Always log meaningful events; suppress the ping/pong keepalive spam
    // unless verbose logging is enabled.
    bool is_keepalive = (strcmp(type_str, "ping") == 0) || (strcmp(type_str, "pong") == 0);
    if (s_log_verbose || !is_keepalive) {
        ESP_LOGI(TAG, "rx: type=%s len=%d", type_str, len);
    }

    // Anything other than the keepalive cadence counts as "server is
    // responding to our turn" — clear the stale-turn watchdog.
    if (strcmp(type_str, "ping") != 0 && strcmp(type_str, "pong") != 0) {
        // DIAGNOSTIC (for the "lower the recovery time" investigation, PROGRESS.md):
        // log the FIRST server event after a PTT close and how long it took.
        // s_ptt_close_us is non-zero only for that first event (cleared just
        // below), so this fires once per turn. Gathered over many turns it
        // answers: is `user_transcript` reliably the first event, and how far
        // ahead of the 16 s give-up does it land? If it's consistently first
        // and early, recovery can key off its ABSENCE instead of waiting 16 s.
        int64_t pclose = s_ptt_close_us;
        if (pclose > 0) {
            ESP_LOGI(TAG, "turn: 1st server event after PTT close = %s at +%lldms "
                          "(recover timeout is %d ms)",
                     type_str,
                     (long long)((esp_timer_get_time() - pclose) / 1000),
                     PTT_STALE_TURN_RECOVER_MS);
        }
        s_ptt_close_us = 0;
        s_stale_warned = false;
        s_stale_recovered = false;
        s_pad_resend_request = false;
    }

    if (strcmp(type_str, "audio") == 0) {
        const cJSON *audio_evt = cJSON_GetObjectItemCaseSensitive(root, "audio_event");
        if (audio_evt) handle_audio_event(audio_evt);
    } else if (strcmp(type_str, "user_transcript") == 0) {
        const cJSON *txt = cJSON_GetObjectItemCaseSensitive(
            cJSON_GetObjectItemCaseSensitive(root, "user_transcription_event"),
            "user_transcript");
        ESP_LOGI(TAG, "user: %s",
                 (cJSON_IsString(txt) && txt->valuestring) ? txt->valuestring : "");
    } else if (strcmp(type_str, "agent_response") == 0) {
        const cJSON *txt = cJSON_GetObjectItemCaseSensitive(
            cJSON_GetObjectItemCaseSensitive(root, "agent_response_event"),
            "agent_response");
        ESP_LOGI(TAG, "agent: %s",
                 (cJSON_IsString(txt) && txt->valuestring) ? txt->valuestring : "");
        // ElevenLabs sends this with the LAST audio chunk → authoritative
        // "no more audio coming this turn". Playback_task uses this to
        // know when it's safe to flip to ORB_MUTED.
        s_agent_done_signal = true;
    } else if (strcmp(type_str, "interruption") == 0) {
        // Agent was interrupted by us — flush queued audio so we stop
        // playing the now-stale response. Clear the done flag too; this
        // turn is being aborted, not completed.
        ESP_LOGI(TAG, "interruption — flushing %u PCM bytes",
                 (unsigned)xStreamBufferBytesAvailable(s_pcm));
        xStreamBufferReset(s_pcm);
        s_agent_done_signal = false;
    } else if (strcmp(type_str, "ping") == 0) {
        s_pings_received++;
        // If PTT is held, mic_task is hammering the WS lock with audio frames
        // every 30 ms. Replying with a pong here would contend on that lock
        // and routinely fail with "Could not lock ws-client within 50 timeout"
        // — and a failed pong-send is logged as such, while a *skipped* pong
        // is harmless: ElevenLabs only enforces "client must respond to ping",
        // and waiting a few seconds (the duration of a PTT) is well within
        // tolerance per legacy Pi behavior.
        if (s_ptt_held) {
            s_pongs_skipped_ptt++;
            cJSON_Delete(root);
            return;
        }
        const cJSON *ping_evt = cJSON_GetObjectItemCaseSensitive(root, "ping_event");
        const cJSON *eid = cJSON_GetObjectItemCaseSensitive(ping_evt, "event_id");
        if (cJSON_IsNumber(eid)) {
            cJSON *pong = cJSON_CreateObject();
            cJSON_AddStringToObject(pong, "type", "pong");
            cJSON_AddNumberToObject(pong, "event_id", eid->valueint);
            char *p = cJSON_PrintUnformatted(pong);
            cJSON_Delete(pong);
            if (p) {
                int n = esp_websocket_client_send_text(s_ws, p, strlen(p), pdMS_TO_TICKS(1000));
                if (n > 0) {
                    s_last_tx_us = esp_timer_get_time();
                    s_tx_bytes_total += n;
                    s_pongs_sent++;
                } else {
                    s_pong_send_failed++;
                    ESP_LOGW(TAG, "pong send failed (rc=%d, eid=%d) — server will tear us down",
                             n, eid->valueint);
                }
                free(p);
            }
        }
    } else if (strcmp(type_str, "conversation_initiation_metadata") == 0) {
        ESP_LOGI(TAG, "init metadata received — ready");
    } else {
        int snip = len < 120 ? len : 120;
        ESP_LOGI(TAG, "evt: %s (%d bytes): %.*s", type_str, len, snip, data);
    }

    cJSON_Delete(root);
}

// Build complete WS messages from possibly-fragmented DATA callbacks. ESP-IDF
// fires WEBSOCKET_EVENT_DATA once per network read, which may be smaller than
// the full WS message; payload_offset/payload_len + the fin bit tell us how
// far along we are.
static void ws_data_chunk(const esp_websocket_event_data_t *evt)
{
    // Any byte from the server (control frames included) refreshes rx age —
    // it's evidence the WS is alive, even if the payload isn't useful to us.
    s_last_rx_us = esp_timer_get_time();
    if (evt->data_len > 0) s_rx_bytes_total += evt->data_len;

    // op_code semantics from ESP-IDF:
    //   first chunk of a message: op_code = real type (1=text, 2=binary)
    //   continuation chunks:      op_code = 0x00
    //   close frame:              op_code = 0x08, pings/pongs 0x09/0x0A
    if (evt->op_code == 0x08) {
        ESP_LOGW(TAG, "ws close frame (payload_len=%d)", evt->payload_len);
        return;
    }
    if (evt->op_code != 0x00 && evt->op_code != 0x01 && evt->op_code != 0x02) {
        // ping/pong/control — ignore (esp_websocket_client handles them).
        return;
    }

    if (evt->payload_offset == 0) {
        s_reasm_len = 0;
        s_reasm_op  = evt->op_code;
    }

    if (s_reasm_len + evt->data_len > REASM_SIZE) {
        ESP_LOGW(TAG, "reasm overflow (offset=%d data=%d size=%d) — dropping",
                 evt->payload_offset, evt->data_len, REASM_SIZE);
        s_reasm_len = 0;
        return;
    }
    memcpy(s_reasm + evt->payload_offset, evt->data_ptr, evt->data_len);
    s_reasm_len = evt->payload_offset + evt->data_len;

    // Message complete when we've collected payload_len total bytes.
    if (s_reasm_len >= evt->payload_len && evt->payload_len > 0) {
        if (s_reasm_len > REASM_SIZE / 2) {
            // Heads-up while tuning REASM_SIZE — long agent turns push the
            // buffer hard. If we're routinely above half, time to grow it.
            ESP_LOGI(TAG, "ws message %d bytes (reasm capacity %d)",
                     s_reasm_len, REASM_SIZE);
        }
        if (s_reasm_op == 0x01) {
            process_message((const char *)s_reasm, s_reasm_len);
        } else if (s_reasm_op == 0x02) {
            ESP_LOGI(TAG, "ws binary (%d bytes) — unexpected for ConvAI", s_reasm_len);
        }
        s_reasm_len = 0;
    }
}

static void send_initiation(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "conversation_initiation_client_data");

    cJSON *cfg = cJSON_AddObjectToObject(root, "conversation_config_override");
    cJSON *agent = cJSON_AddObjectToObject(cfg, "agent");
    (void)agent;

    cJSON *audio_in = cJSON_AddObjectToObject(cfg, "input_audio_format");
    cJSON_AddStringToObject(audio_in, "format", "pcm");
    cJSON_AddNumberToObject(audio_in, "sample_rate", 16000);

    cJSON *audio_out = cJSON_AddObjectToObject(cfg, "output_audio_format");
    cJSON_AddStringToObject(audio_out, "format", "pcm");
    cJSON_AddNumberToObject(audio_out, "sample_rate", 16000);

    // Pin server VAD silence threshold to 800 ms (= ElevenLabs default) so
    // it can't drift under us. PTT_TAIL_SILENCE_FRAMES feeds 1290 ms of
    // PCM-zero pad after release → 490 ms margin over this threshold.
    cJSON *turn = cJSON_AddObjectToObject(cfg, "turn_detection");
    cJSON_AddNumberToObject(turn, "silence_duration_ms", 800);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) return;

    int n = esp_websocket_client_send_text(s_ws, payload, strlen(payload), pdMS_TO_TICKS(2000));
    if (n > 0) { s_last_tx_us = esp_timer_get_time(); s_tx_bytes_total += n; }
    ESP_LOGI(TAG, "init sent (%d bytes)", (int)strlen(payload));
    free(payload);
}

// Build a "<n>ms" age string for last-rx / last-tx, or "never" if the
// timestamp was never set.
static void age_ms_str(int64_t when_us, char *out, size_t out_sz)
{
    if (when_us == 0) { snprintf(out, out_sz, "never"); return; }
    int64_t age_ms = (esp_timer_get_time() - when_us) / 1000;
    snprintf(out, out_sz, "%lldms", (long long)age_ms);
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_websocket_event_data_t *evt = (esp_websocket_event_data_t *)event_data;
    switch (id) {
        case WEBSOCKET_EVENT_CONNECTED:
            s_ws_connect_count++;
            ESP_LOGI(TAG, "ws connected (#%u)", (unsigned)s_ws_connect_count);
            s_reasm_len = 0;
            send_initiation();
            // WSS connected = WiFi+DNS+TLS+agent-auth all proven on this
            // build. Mark the running image valid so the bootloader stops
            // tracking it as PENDING_VERIFY. No-op when not on a fresh OTA.
            orb_ota_mark_running_valid();
            break;
        case WEBSOCKET_EVENT_DISCONNECTED: {
            s_ws_disconnect_count++;
            char rx_age[24], tx_age[24];
            age_ms_str(s_last_rx_us, rx_age, sizeof(rx_age));
            age_ms_str(s_last_tx_us, tx_age, sizeof(tx_age));
            // error_handle is set on both errors and clean disconnects; dump
            // whatever is non-zero so silent close vs TLS-error vs handshake-
            // status all show up distinctly.
            ESP_LOGW(TAG, "ws disconnected (#%u): rx_age=%s tx_age=%s last=%s "
                          "err_type=%d esp_tls_err=0x%x tls_stack=0x%x sock_errno=%d hs_status=%d",
                     (unsigned)s_ws_disconnect_count, rx_age, tx_age,
                     s_last_msg_type[0] ? s_last_msg_type : "-",
                     evt->error_handle.error_type,
                     evt->error_handle.esp_tls_last_esp_err,
                     evt->error_handle.esp_tls_stack_err,
                     evt->error_handle.esp_transport_sock_errno,
                     evt->error_handle.esp_ws_handshake_status_code);
            // Drop any queued send frames — they're for the dead session and
            // would otherwise replay against the reconnected session's
            // greeting context, which would confuse the server.
            if (s_queue) {
                size_t drained = 0, item_sz = 0;
                void *p;
                while ((p = xRingbufferReceive(s_queue, &item_sz, 0)) != NULL) {
                    vRingbufferReturnItem(s_queue, p);
                    drained++;
                }
                if (drained) {
                    ESP_LOGW(TAG, "ws disconnect — dropped %u queued frames "
                                  "(bound for dead session)", (unsigned)drained);
                }
                // Reset depth tracking — bytes_in/out semantics break if
                // we drained without crediting s_q_bytes_out.
                s_q_bytes_in  = 0;
                s_q_bytes_out = 0;
            }
            break;
        }
        case WEBSOCKET_EVENT_DATA:
            ws_data_chunk(evt);
            break;
        case WEBSOCKET_EVENT_ERROR: {
            char rx_age[24], tx_age[24];
            age_ms_str(s_last_rx_us, rx_age, sizeof(rx_age));
            age_ms_str(s_last_tx_us, tx_age, sizeof(tx_age));
            ESP_LOGE(TAG, "ws error: rx_age=%s tx_age=%s last=%s "
                          "err_type=%d esp_tls_err=0x%x tls_stack=0x%x sock_errno=%d",
                     rx_age, tx_age,
                     s_last_msg_type[0] ? s_last_msg_type : "-",
                     evt->error_handle.error_type,
                     evt->error_handle.esp_tls_last_esp_err,
                     evt->error_handle.esp_tls_stack_err,
                     evt->error_handle.esp_transport_sock_errno);
            break;
        }
        default:
            break;
    }
}

// ─── Heartbeat ───────────────────────────────────────────────────────────
// Single-line status every 2s. Reading the UDP log as a timeline, you can
// spot: WS silently going DOWN (ws=DOWN), TLS rx hangs (rx_age climbing
// without a disconnect), server-message droughts (last=ping for 30s), LVGL
// task freeze (lvgl_d=0 between heartbeats), heap leaks (heap shrinking).
//
// Also acts as a periodic "user_activity" keepalive scheduler: every 60 s
// the heartbeat sends a `user_activity` JSON event, matching the legacy
// Pi's maintain_pong loop. This proactively keeps the connection alive
// independent of the server's ping cadence — if the server's ping interval
// ever drifts past our inactivity_timeout we'd otherwise die silently.
#define USER_ACTIVITY_INTERVAL_MS  60000

static void heartbeat_task(void *arg)
{
    uint32_t prev_lvgl = LVGL_GetIterCounter();
    uint32_t prev_rx_total = 0;
    int64_t  last_user_activity_us = 0;
    while (s_running) {
        vTaskDelay(pdMS_TO_TICKS(2000));

        char rx_age[24], tx_age[24];
        age_ms_str(s_last_rx_us, rx_age, sizeof(rx_age));
        age_ms_str(s_last_tx_us, tx_age, sizeof(tx_age));

        uint32_t lvgl_now = LVGL_GetIterCounter();
        uint32_t lvgl_delta = lvgl_now - prev_lvgl;
        prev_lvgl = lvgl_now;

        uint32_t rx_now = s_rx_bytes_total;
        uint32_t rx_delta = rx_now - prev_rx_total;
        prev_rx_total = rx_now;

        bool ws_conn = s_ws && esp_websocket_client_is_connected(s_ws);
        size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t free_psram    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

        // pings_received - pongs_sent - skipped_during_ptt should be 0 (or
        // 1 transient if a ping is mid-handling at the moment we sample).
        // Anything else means pongs were *unintentionally* dropped and the
        // server will time us out. Subtracting skip_ptt keeps the metric
        // clean: by-design skips (during PTT, see ping handler) don't
        // pollute it.
        uint32_t pings = s_pings_received;
        uint32_t pongs = s_pongs_sent;
        uint32_t pong_fail = s_pong_send_failed;
        int32_t  pp_delta = (int32_t)pings - (int32_t)pongs - (int32_t)s_pongs_skipped_ptt;

        // Send queue stats — depth (bytes currently waiting) tells us if the
        // sender is keeping up; block + retry counters are cumulative
        // session totals (rises = transient congestion happening).
        uint32_t bytes_in  = s_q_bytes_in;
        uint32_t bytes_out = s_q_bytes_out;
        uint32_t q_depth_bytes = (bytes_in > bytes_out) ? (bytes_in - bytes_out) : 0;

        if (s_log_verbose) {
            ESP_LOGI(TAG, "hb: ws=%s rx_age=%s tx_age=%s rx_2s=%uB last=%s "
                          "lvgl_d=%u ptt=%d heap_int=%uK psram=%uK conn#%u disc#%u "
                          "pings=%u pongs=%u dropped=%u skip_ptt=%u pp_delta=%d "
                          "q_depth=%uB q_block=%u q_retry=%u",
                     ws_conn ? "up" : "DOWN", rx_age, tx_age,
                     (unsigned)rx_delta,
                     s_last_msg_type[0] ? s_last_msg_type : "-",
                     (unsigned)lvgl_delta,
                     s_ptt_held ? 1 : 0,
                     (unsigned)(free_internal / 1024),
                     (unsigned)(free_psram / 1024),
                     (unsigned)s_ws_connect_count,
                     (unsigned)s_ws_disconnect_count,
                     (unsigned)pings, (unsigned)pongs,
                     (unsigned)pong_fail,
                     (unsigned)s_pongs_skipped_ptt,
                     (int)pp_delta,
                     (unsigned)q_depth_bytes,
                     (unsigned)s_queue_block_count,
                     (unsigned)s_send_retries);
        }

        // Periodic user_activity keepalive. Skip while PTT is held (mic_task
        // is using the WS) and only when the WS is up.
        int64_t now_us = esp_timer_get_time();
        bool first_run = (last_user_activity_us == 0);
        bool due = !first_run &&
                   ((now_us - last_user_activity_us) >= (int64_t)USER_ACTIVITY_INTERVAL_MS * 1000);
        if ((first_run || due) && ws_conn && !s_ptt_held) {
            const char *ua = "{\"type\":\"user_activity\"}";
            int n = esp_websocket_client_send_text(s_ws, (char *)ua, strlen(ua),
                                                   pdMS_TO_TICKS(500));
            if (n > 0) {
                s_last_tx_us = esp_timer_get_time();
                s_tx_bytes_total += n;
                last_user_activity_us = now_us;
            } else {
                ESP_LOGW(TAG, "user_activity keepalive send failed (rc=%d)", n);
                last_user_activity_us = now_us;   // back off until next interval
            }
        }

        // Stale-turn watchdog: the server received our turn but sent nothing
        // but pings back — it missed the end-of-turn. Two-stage recovery,
        // both one-shot, both keyed off the PTT-close timestamp (cleared by
        // process_message the instant any real server event arrives, which
        // cancels whichever stage hasn't fired yet).
        int64_t pclose = s_ptt_close_us;
        if (pclose > 0) {
            int64_t age_ms = (esp_timer_get_time() - pclose) / 1000;
            // Stage 1 — WARN + nudge: ask mic_task to re-send the tail pad to
            // re-trigger the server VAD. Cheap and safe; often unsticks it.
            if (age_ms > PTT_STALE_TURN_WARN_MS && !s_stale_warned) {
                ESP_LOGW(TAG, "TURN STALE: %lldms since PTT close, no server "
                              "response (last=%s, ws=%s). Server likely missed "
                              "end-of-turn — re-sending tail pad to nudge VAD.",
                         (long long)age_ms,
                         s_last_msg_type[0] ? s_last_msg_type : "-",
                         ws_conn ? "up" : "DOWN");
                s_pad_resend_request = true;
                s_stale_warned = true;
            }
            // Stage 2 — give up: nudge didn't land. Don't leave the user
            // staring at a frozen ORB_LOADING; show ORB_RETRY ("No response /
            // Please repeat") and clear the watchdog. ORB_RETRY is an idle-
            // equivalent state (PTT + NFC both work), so it persists until the
            // user presses to try again — a press flips the UI to ORB_USER_TALK
            // normally. Safe even if a late response arrives — its audio will
            // flip the UI to ORB_AGENT.
            if (age_ms > PTT_STALE_TURN_RECOVER_MS && !s_stale_recovered) {
                ESP_LOGW(TAG, "TURN STALE: %lldms, no response after nudge — "
                              "giving up, prompting user to repeat (ORB_RETRY).",
                         (long long)age_ms);
                s_stale_recovered = true;
                s_pad_resend_request = false;
                s_ptt_close_us = 0;
                orb_ui_set_state(ORB_RETRY);
                // Audible cue so the user notices even if not looking at the
                // orb: the NFC blopp's top tone (350 Hz) played twice in quick
                // succession. i2s_audio_beep is synchronous, so the vTaskDelay
                // gives a clean gap between the two beeps. Safe here — the PCM
                // ring is empty (no response arrived), so no playback contends.
                i2s_audio_beep(350, 90);
                vTaskDelay(pdMS_TO_TICKS(70));
                i2s_audio_beep(350, 90);
            }
        }
    }
    vTaskDelete(NULL);
}

// ─── Lifecycle ───────────────────────────────────────────────────────────

bool convai_start(const char *agent_id)
{
    if (s_running) {
        ESP_LOGW(TAG, "start: already running");
        return false;
    }
    if (!agent_id || !agent_id[0]) {
        ESP_LOGE(TAG, "start: empty agent_id");
        return false;
    }

    // Allocate the reassembly buffer + PCM ring buffer in PSRAM. Both are
    // released by convai_stop() — re-allocate fresh on every conversation.
    s_reasm = heap_caps_malloc(REASM_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_reasm) {
        ESP_LOGE(TAG, "start: PSRAM alloc reasm failed");
        return false;
    }
    s_pcm_storage = heap_caps_malloc(PCM_RING_BYTES + 1, MALLOC_CAP_SPIRAM);
    if (!s_pcm_storage) {
        ESP_LOGE(TAG, "start: PSRAM alloc pcm failed");
        free(s_reasm); s_reasm = NULL;
        return false;
    }
    s_pcm = xStreamBufferCreateStatic(PCM_RING_BYTES, sizeof(int16_t),
                                       s_pcm_storage, &s_pcm_struct);
    if (!s_pcm) {
        ESP_LOGE(TAG, "start: stream buffer create failed");
        free(s_reasm); s_reasm = NULL;
        free(s_pcm_storage); s_pcm_storage = NULL;
        return false;
    }

    strncpy(s_agent_id, agent_id, sizeof(s_agent_id) - 1);
    s_agent_id[sizeof(s_agent_id) - 1] = '\0';

    char uri[256];
    snprintf(uri, sizeof(uri), WS_URI_FMT, s_agent_id);

    static char hdr[160];
    snprintf(hdr, sizeof(hdr), "xi-api-key: %s\r\n", EL_API_KEY);

    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .headers = hdr,
        .network_timeout_ms = WS_NETWORK_TIMEOUT_MS,
        .reconnect_timeout_ms = WS_RECONNECT_DELAY_MS,
        .disable_auto_reconnect = false,
        .cert_pem = gts_root_r1_pem_start,
        .cert_len = gts_root_r1_pem_end - gts_root_r1_pem_start,
        .buffer_size = WS_BUFFER_SIZE,
        // WS task stack must come out of *contiguous* internal SRAM. By the
        // time convai_start runs (after WiFi/lwIP/mbedTLS init), the heap is
        // fragmented — sample probe showed total free internal=24 KB but
        // largest contiguous block=7.5 KB. 8 KB stack therefore fails to
        // allocate even though we'd nominally have room. 6 KB fits in the
        // fragmented block and still gives the event handler ~2 KB margin
        // over its observed worst case (cJSON parse + 768 B PCM buffer +
        // base64 decode ≈ 3-4 KB peak).
        .task_stack = 6144,
    };

    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) {
        ESP_LOGE(TAG, "start: client_init failed");
        return false;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);

    // Heap probe before the WS lib spawns its internal task (needs
    // task_stack=8192 of *internal* SRAM). "Error create websocket task"
    // from the lib means this number was too low at this moment.
    ESP_LOGI(TAG, "start: heap_int=%u largest_int=%u psram=%u before client_start",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    if (esp_websocket_client_start(s_ws) != ESP_OK) {
        ESP_LOGE(TAG, "start: client_start failed (heap_int=%u largest=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        return false;
    }

    // Mic streaming buffers (PSRAM — would blow the heap if we put 24 KB
    // on internal RAM per chunk). One allocation, reused for every chunk.
    s_mic_pcm  = heap_caps_malloc(MIC_CHUNK_BYTES, MALLOC_CAP_SPIRAM);
    s_mic_b64  = heap_caps_malloc(MIC_B64_CAP,    MALLOC_CAP_SPIRAM);
    s_mic_json = heap_caps_malloc(MIC_JSON_CAP,   MALLOC_CAP_SPIRAM);
    if (!s_mic_pcm || !s_mic_b64 || !s_mic_json) {
        ESP_LOGE(TAG, "start: PSRAM alloc for mic buffers failed");
    }

    // Fresh start — clear any end-of-turn state left over from a previous
    // session torn down via convai_stop().
    s_agent_done_signal = false;
    s_last_audio_rx_us  = 0;
    s_agent_speaking    = false;
    s_turn_response_started = false;
    s_queue_block_count = 0;
    s_send_retries      = 0;
    s_q_bytes_in        = 0;
    s_q_bytes_out       = 0;

    // Force verbose ON for this debugging window so the per-2s heartbeat
    // (including q_depth / q_block / q_retry) appears in the UDP log
    // without anyone needing to call convai_set_log_verbose. Remove once
    // the send queue is confirmed working.
    s_log_verbose = true;

    // Send queue (PSRAM-backed ringbuffer + sender task). Created BEFORE
    // s_running flips true so the sender task doesn't race with allocation.
    s_queue_storage = heap_caps_malloc(SEND_QUEUE_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_queue_storage) {
        ESP_LOGE(TAG, "start: PSRAM alloc send queue (%d bytes) failed",
                 SEND_QUEUE_BYTES);
        // Don't fail-out — the queue is a robustness improvement; without
        // it we revert to direct-send behaviour (convai_send_enqueue
        // returns -1, mic_send_chunk_with_status returns -1, mic_task
        // continues; no audio is sent until queue is up). Log and proceed
        // so existing functionality isn't blocked by an alloc failure.
    } else {
        s_queue = xRingbufferCreateStatic(SEND_QUEUE_BYTES, RINGBUF_TYPE_NOSPLIT,
                                          s_queue_storage, &s_queue_struct);
        if (!s_queue) {
            ESP_LOGE(TAG, "start: send queue create failed");
            free(s_queue_storage);
            s_queue_storage = NULL;
        }
    }

    s_running = true;

    // Spawn order matters: by the time we get here, the heap is already
    // fragmented (heap probe shows ~12 KB largest contiguous). Each 4 KB
    // task stack further fragments. Put the sender's stack in PSRAM via
    // xTaskCreatePinnedToCoreWithCaps so it can't fail due to internal-
    // RAM fragmentation. The other three tasks stay in internal RAM
    // because they're already known to work.
    if (s_queue) {
        // 8 KB (not 4 KB): sender_task calls esp_websocket_client_send_text →
        // mbedTLS TLS-record encryption, which is stack-hungry. At 4 KB it ran
        // with ~52 bytes free on the clean send path and OVERFLOWED the moment a
        // send hit the contended/partial-write path (transport_poll_write(0)) —
        // confirmed by coredump (task "convai_send", 4144/52). Deeper send paths
        // are reachable any time the WS transport is congested; NFC custom-phrase
        // injects made it reliable because they burst frames with pong NOT
        // skipped (s_ptt_held==false). The WS lib's own task runs this same path
        // at 6 KB; 8 KB gives comfortable margin. PSRAM stack, so no SRAM cost.
        BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
            sender_task, "convai_send", 8192, NULL, 5,
            &s_sender_task, 0, MALLOC_CAP_SPIRAM);
        if (rc != pdPASS) {
            ESP_LOGE(TAG, "start: sender_task spawn FAILED (rc=%d, "
                          "heap_int=%u largest=%u psram=%u) — mic frames "
                          "will queue but never be sent",
                     (int)rc,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            s_sender_task = NULL;
        }
    } else {
        ESP_LOGE(TAG, "start: send queue is NULL — sender_task NOT spawned, "
                      "mic frames will be dropped");
    }
    // Playback task pulls from the PCM ring and drives the speaker. Its stack
    // lives in PSRAM for the SAME reason as sender/mic/heartbeat below — by the
    // time we get here internal SRAM is too fragmented to reliably fit a 4 KB
    // stack, and the WS lib task just claimed the largest internal block. It
    // used to spawn in internal RAM on the assumption that "one 4 KB internal
    // stack still fits"; that assumption is false on release builds (the OTA
    // GitHub handshake lowers heap_int ~7 KB vs a dev build that skips OTA), so
    // it intermittently failed to spawn → no audio + UI wedged on ORB_LOADING.
    // The PCM ring and I2S DMA buffers are already PSRAM; the stack only holds
    // locals, so PSRAM is safe (the task simply isn't scheduled during the rare
    // cache-disabled windows, same as the other three).
    //
    // This is the one task the session cannot function without, so if it STILL
    // can't spawn we fail the whole start: tear everything down and return false
    // so the caller retries instead of leaving a half-dead, audio-less session
    // up (WS connected, greeting received, nothing to play it, UI stuck).
    {
        BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
            playback_task, "convai_play", 4096, NULL, 5,
            &s_playback_task, 1, MALLOC_CAP_SPIRAM);
        if (rc != pdPASS) {
            ESP_LOGE(TAG, "start: playback_task spawn FAILED (rc=%d, "
                          "heap_int=%u largest=%u psram=%u) — aborting session",
                     (int)rc,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            s_playback_task = NULL;
            convai_stop();   // full teardown: s_running=false, WS close/destroy,
                             // free PSRAM buffers, self-exit sender_task
            return false;
        }
    }
    // Mic and heartbeat task stacks live in PSRAM (same pattern as
    // sender_task above) — by the time convai_start runs, the OTA path has
    // already done two TLS handshakes (Supabase + GitHub) plus the WS task
    // just claimed the largest internal-SRAM block, leaving fragments too
    // small for 4 KB / 3 KB internal allocations. Without PSRAM these
    // tasks silently fail to spawn and PTT does nothing (mic_task never
    // reads s_ptt_held).
    {
        BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
            mic_task, "convai_mic", 4096, NULL, 5,
            &s_mic_task, 1, MALLOC_CAP_SPIRAM);
        if (rc != pdPASS) {
            ESP_LOGE(TAG, "start: mic_task spawn FAILED (rc=%d, "
                          "heap_int=%u largest=%u psram=%u)",
                     (int)rc,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            s_mic_task = NULL;
        }
    }
    // Heartbeat: low-priority background task, prints one diagnostic line
    // every 2s for the lifetime of the conversation. Stack is small —
    // it only calls esp_log + heap_caps + atomic reads.
    {
        // 8 KB (not 3 KB): heartbeat_task sends the 60 s user_activity keepalive
        // via the same esp_websocket_client_send_text → mbedTLS write path that
        // overflowed sender_task's 4 KB stack. Same latent defect on an even
        // smaller stack; bump it too rather than wait for the next coredump.
        // PSRAM stack, so no internal-SRAM cost.
        BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
            heartbeat_task, "convai_hb", 8192, NULL, 1,
            &s_heartbeat_task, 0, MALLOC_CAP_SPIRAM);
        if (rc != pdPASS) {
            ESP_LOGE(TAG, "start: heartbeat_task spawn FAILED (rc=%d)", (int)rc);
            s_heartbeat_task = NULL;
        }
    }

    ESP_LOGI(TAG, "start: connecting to %s", uri);
    return true;
}

void convai_stop(void)
{
    if (!s_running) return;
    s_running = false;          // signals mic_task to exit its loop
    s_ptt_held = false;
    if (s_playback_task) {
        vTaskDelete(s_playback_task);
        s_playback_task = NULL;
    }
    // mic_task + heartbeat_task + sender_task self-delete when s_running
    // goes false. Wait briefly so mic_task can call i2s_audio_record_stop()
    // and sender_task can finish its current xRingbufferReceive() poll.
    vTaskDelay(pdMS_TO_TICKS(600));
    s_mic_task = NULL;
    s_heartbeat_task = NULL;
    s_sender_task = NULL;
    if (s_ws) {
        esp_websocket_client_close(s_ws, pdMS_TO_TICKS(2000));
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    if (s_pcm)         { vStreamBufferDelete(s_pcm); s_pcm = NULL; }
    if (s_pcm_storage) { free(s_pcm_storage); s_pcm_storage = NULL; }
    if (s_reasm)       { free(s_reasm);       s_reasm = NULL; }
    if (s_mic_pcm)     { free(s_mic_pcm);     s_mic_pcm  = NULL; }
    if (s_mic_b64)     { free(s_mic_b64);     s_mic_b64  = NULL; }
    if (s_mic_json)    { free(s_mic_json);    s_mic_json = NULL; }
    if (s_queue)       { vRingbufferDelete(s_queue); s_queue = NULL; }
    if (s_queue_storage) { free(s_queue_storage); s_queue_storage = NULL; }
    s_reasm_len = 0;
    ESP_LOGI(TAG, "stop: torn down");
}

bool convai_send_user_message(const char *text)
{
    if (!s_running || !s_ws || !text || !text[0]) return false;
    if (s_ptt_held) {
        // Must not interleave with a live mic capture (shares s_mic_pcm with the
        // silence pad below). NFC only scans while muted so this shouldn't fire,
        // but guard anyway.
        ESP_LOGW(TAG, "user_message skipped: PTT held");
        return false;
    }

    // Build {"type":"user_message","text":"<text>"} with cJSON so the phrase is
    // correctly JSON-escaped (it can contain quotes/UTF-8).
    cJSON *root = cJSON_CreateObject();
    if (!root) return false;
    cJSON_AddStringToObject(root, "type", "user_message");
    cJSON_AddStringToObject(root, "text", text);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) return false;

    // Direct send (not via the mic ring buffer) — this is a rare one-shot, same
    // class as pong / user_activity / send_initiation.
    int n = esp_websocket_client_send_text(s_ws, payload, strlen(payload), pdMS_TO_TICKS(2000));
    if (n > 0) { s_last_tx_us = esp_timer_get_time(); s_tx_bytes_total += n; }
    ESP_LOGI(TAG, "user_message injected (rc=%d): \"%s\"", n, text);
    free(payload);
    if (n <= 0) return false;

    // Force the user turn to end so the agent actually responds. The session
    // uses server-side VAD turn detection on the *audio* stream
    // (turn_detection.silence_duration_ms in send_initiation), so a text-only
    // user_message gives the server no end-of-turn and the agent stays silent
    // (symptom: stuck on ORB_LOADING). Append the same tail-silence pad a PTT
    // release sends — BEHAVIOR.md §6.2 / §3.5 "force_turn_end" — so the VAD
    // closes the turn. Also arm the stale-turn watchdog like the PTT path does.
    //
    // The pad is ABORTABLE: it stops the instant the agent's audio starts
    // arriving (s_turn_response_started). By then the server has already closed
    // the turn, so the remaining frames are wasted — and worse, they'd fight
    // the WS-lib recursive lock that handle_audio_event holds while draining
    // that audio, which is what caused the post-inject WS disconnect. Clear the
    // flag first so a fresh arrival (not a stale one) is what trips the abort.
    s_ptt_close_us = esp_timer_get_time();
    s_stale_warned = false;
    s_stale_recovered = false;
    s_pad_resend_request = false;
    s_turn_response_started = false;
    send_silence_frames(PTT_TAIL_SILENCE_FRAMES, true);
    return true;
}

bool convai_is_running(void)
{
    return s_running;
}

void convai_set_log_verbose(bool verbose)
{
    s_log_verbose = verbose;
}
