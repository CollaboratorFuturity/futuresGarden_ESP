# ConvAI (ElevenLabs WebSocket) — full reference

Sister doc to [SCREEN.md](SCREEN.md). Short-form rules live in
[CLAUDE.md → "ElevenLabs WSS"](CLAUDE.md). This file is the full reference
for the WS + send queue + audio paths in [main/Convai/convai.c](main/Convai/convai.c).

If something in this stack regresses, **diff `main/Convai/convai.c` against
the legacy Pi reference** at [futuresGarden/main.py](futuresGarden/main.py).
Most of our reliability features are direct ports of Pi behaviour.

---

## What this code is responsible for

1. Maintaining one persistent `wss://api.elevenlabs.io/v1/convai/conversation`
   WebSocket per conversation (started on NFC scan, torn down on the next
   scan or `convai_stop`).
2. Streaming mic audio to the server during PTT (~30 ms frames, 16 kHz, mono,
   PCM → base64 → JSON).
3. Receiving agent audio + transcripts, reassembling fragmented WS messages,
   decoding base64 to PCM, driving the speaker.
4. Replying to server pings and sending periodic `user_activity` so the
   connection doesn't time out.
5. Driving the orb UI state through the conversation lifecycle (loading →
   user_talk → loading → agent → muted).
6. Surfacing enough telemetry over the UDP log sink that a half-broken
   session is visible at a glance.

---

## High-level data flow

```
        ┌─────────────────────────────────────────────────────────────────┐
        │                          INBOUND                                │
        │                                                                 │
   WS ──┼──► ws_data_chunk ──► reassembly buf (2 MB PSRAM) ──► process_message
        │                                                          │      │
        │                                                          ▼      │
        │                                                   handle_audio  │
        │                                                          │      │
        │                                                          ▼      │
        │                                            base64 decode (1 KB) │
        │                                                          │      │
        │                                                          ▼      │
        │                                              PCM ring (128 KB)  │
        │                                                          │      │
        │                                                          ▼      │
        │                                          playback_task ──► I2S  │
        └─────────────────────────────────────────────────────────────────┘

        ┌─────────────────────────────────────────────────────────────────┐
        │                          OUTBOUND                               │
        │                                                                 │
   mic ─► I2S RX ──► mic_task ──► base64+JSON encode ──┐                 │
        │                                              │                 │
        │              (30 ms cadence)                 ▼                 │
        │                                  ┌─ send queue (1 MB PSRAM) ─┐ │
        │                                  │   xRingbuffer NOSPLIT     │ │
        │                                  │   FIFO, ≈ 800 items max   │ │
        │                                  └────────────┬──────────────┘ │
        │                                               │                 │
        │                                               ▼                 │
        │                                       sender_task               │
        │                                       (PSRAM stack, core 0)     │
        │                                       send + 500 ms retry       │
        │                                               │                 │
        │   pong, user_activity, initiation ────────┐   │                 │
        │   (direct, bypass queue)                  │   │                 │
        │                                           ▼   ▼                 │
        │                              esp_websocket_client_send_text     │
        │                                               │                 │
        └───────────────────────────────────────────────┼─────────────────┘
                                                        ▼
                                                       WS
```

---

## File map

| Symbol | What it does | File:line approx |
|---|---|---|
| `convai_start(agent_id)` | Allocate everything, create WS, spawn tasks. Idempotent guard via `s_running`. | `convai.c` lifecycle section |
| `convai_stop()` | Tear down WS, free buffers, kill tasks. Called from NFC re-scan. | same |
| `convai_ptt_set(pressed)` | Button task pokes this on edge transitions. | same |
| `convai_is_running()` | Used by NFC code to decide stop-vs-start. | same |
| `mic_task` | Reads I2S RX in 30 ms frames, gates on PTT held, encodes+enqueues. | mic section |
| `mic_send_chunk_with_status` | base64 → JSON → `convai_send_enqueue`. Returns the enqueued length (was: bytes-actually-sent in the pre-queue era). | mic section |
| `send_silence_frames(n)` | Posts `n` zero-PCM frames at 30 ms cadence after PTT release. | mic section |
| `convai_send_enqueue` | Non-blocking push, blocks on full (never drops). Increments `s_q_bytes_in`. | mic section |
| `sender_task` | Pops from queue, sends, retries on failure with 500 ms backoff. PSRAM stack. | mic section |
| `playback_task` | Reads from PCM ring, drives I2S TX. Tracks `agent_response` arrival for end-of-turn detection. | playback section |
| `handle_audio_event` | Base64-decode incoming audio in 1 KB chunks straight into the PCM ring with `portMAX_DELAY` backpressure. | event handling |
| `process_message` | Dispatches by `type`. Pong gating, agent_response → ORB transition, interruption ring-reset, etc. | event handling |
| `ws_data_chunk` | Reassembles fragmented WS messages into `s_reasm` (2 MB PSRAM). | event handling |
| `send_initiation` | Builds and direct-sends `conversation_initiation_client_data`. Once per WS connect. | lifecycle |
| `ws_event_handler` | Top-level WS state machine. Hooks: CONNECTED, DISCONNECTED, DATA, ERROR. | event handling |
| `heartbeat_task` | Per-2 s telemetry. Also schedules the periodic `user_activity` keepalive. | heartbeat section |

---

## Versions and dependencies

| Component | Pin | Why |
|---|---|---|
| `espressif/esp_websocket_client` | **`1.4.0`** (exact) | v1.5+ calls `esp_transport_ws_get_redir_uri` which isn't in our tcp_transport on IDF 5.5. Newer would require a patch. |
| ESP-IDF | **5.5.0** (locked, see SCREEN.md) | Locked for screen reasons. mbedtls and lwIP versions follow. |
| TLS root | `certs/gts_root_r1.pem` (embedded) | `api.elevenlabs.io` chains to GTS Root R1. The built-in CA bundle didn't match reliably on our build, so we pin it. Rotate via re-flash if it expires. |
| mbedTLS hardware AES | **off** (`CONFIG_MBEDTLS_HARDWARE_AES=n`) | HW AES needs DMA-capable internal SRAM per record; long agent audio events repeatedly `ENOMEM`ed. SW AES is plenty fast for our 43 KB/s upstream. |
| mbedTLS external mem alloc | **on** (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`) | Without it, TLS buffer allocs come from internal SRAM and OOM the handshake. With it, mbedTLS allocates from PSRAM. |

---

## Memory layout (PSRAM-heavy by design)

| Buffer | Size | Pool | Created in | Notes |
|---|---|---|---|---|
| WS reassembly buf | 2 MB | PSRAM | `convai_start` | Holds incoming fragmented WS messages. Single agent audio events have been seen at 250+ KB; this is sized for ~30 s of monologue worst case. |
| PCM ring (`s_pcm`) | 128 KB | PSRAM (static stream buffer) | `convai_start` | Between WS receive and speaker write. ~4 s of audio at 32 KB/s drain rate. Backpressure (`portMAX_DELAY`) means oversized audio events just slow the WS task — never overflow. |
| Send queue (`s_queue`) | 1 MB | PSRAM (static ringbuffer NOSPLIT) | `convai_start` | ~24 s of mic audio at 43 KB/s outbound rate. FIFO, item-oriented. Producer (`mic_task`) blocks on full, never drops. |
| Mic PCM buffer | 960 B | PSRAM | `convai_start` | One I2S frame (30 ms × 16 kHz × 2 B). |
| Mic base64 buffer | 1284 B | PSRAM | `convai_start` | Encoded form of one frame. |
| Mic JSON buffer | 1348 B | PSRAM | `convai_start` | Wrapped form: `{"user_audio_chunk":"<base64>"}`. |
| WS lib task stack | 6144 B | **Internal SRAM** (allocated by the lib) | inside `esp_websocket_client_start` | Was 8192. See "WSS task stack" in CLAUDE.md and the troubleshooting section below. |
| `playback_task` stack | 4096 B | Internal SRAM | `convai_start` | Pinned core 1. The only convai task still in internal SRAM. |
| `mic_task` stack | 4096 B | **PSRAM** (via `xTaskCreatePinnedToCoreWithCaps`) | `convai_start` | **Must be PSRAM** — see troubleshooting. Pinned core 1. |
| `heartbeat_task` stack | 3072 B | **PSRAM** (via `xTaskCreatePinnedToCoreWithCaps`) | `convai_start` | **Must be PSRAM** — see troubleshooting. Pinned core 0. |
| `sender_task` stack | 4096 B | **PSRAM** (via `xTaskCreatePinnedToCoreWithCaps`) | `convai_start` | **Must be PSRAM** — see troubleshooting. Pinned core 0. |

Three of the four convai tasks (`mic_task`, `heartbeat_task`, `sender_task`)
live in PSRAM; only `playback_task` (4 KB) stays in internal SRAM. So total
internal SRAM cost at runtime is **~4 KB for our one internal task stack +
6 KB for the WS lib task**. By the time `convai_start` runs — after the OTA
path's two TLS handshakes and the WS task claiming the largest internal
block — internal heap is fragmented enough that a second/third 4 KB internal
stack allocation fails *silently* (no panic), and PTT would do nothing
because `mic_task` never spawned. Steady-state free internal heap during
heavy audio is **6–12 KB**. There is no room for growth — if you need more
stack anywhere, take it from PSRAM via `xTaskCreatePinnedToCoreWithCaps`.

---

## The app-level send queue — the load-bearing fix

### Why it exists

Pre-queue, `mic_task` called `esp_websocket_client_send_text` directly
every 30 ms. The lib's internal `transport_poll_write` uses a timeout
(`network_timeout_ms = 10000`); if a single write doesn't get the socket
writable within that window, the lib treats it as transport failure,
fires `WEBSOCKET_EVENT_DISCONNECTED`, and auto-reconnects. Auto-reconnect
re-runs `send_initiation`, which the server interprets as a *new
conversation*, and we hear the agent greeting from scratch — what we
were calling "the conversation restarts mid-PTT."

Three distinct root causes, all converging on the same symptom:

| Failure mode | Cause | Preventable? |
|---|---|---|
| `transport_poll_write(0)` | TCP send buffer full, kernel can't accept more bytes | **Yes** — the queue absorbs the back-pressure so we never feed the lib a failing write |
| `errno=104 (ECONNRESET)` | Server (or middlebox) actively closed | No — link is gone, queue can't help. We just don't *cause* it. |
| WiFi STA reassociation | Underlying physical layer dropped | No — same as above |

The queue addresses #1, which is by far the most common.

### Why our lib is fragile here

The Python `websockets` library used on the Pi has `await ws.send()`
which queues asynchronously and back-pressures the producer with `await`.
`esp_websocket_client` v1.4 has no equivalent — `send_text` is
fire-and-fail. So we reproduce the Python lib's queueing semantics in
front of the C lib.

### Design

```
mic_task → encode JSON → convai_send_enqueue
                              │
                              ▼
                  ┌─ xRingbuffer 1 MB PSRAM ─┐
                  │   NOSPLIT mode            │
                  │   FIFO, ~800 items max    │
                  └──────────┬────────────────┘
                              ▼
                       sender_task
                       (PSRAM stack, core 0, prio 5)
                       │
                       ▼
                  esp_websocket_client_send_text
                       │
                       ├── success → consume item, next
                       └── failure → 500 ms backoff, retry SAME item
```

Key properties:

- **One item is sent at a time.** No batching. Each item is the full
  JSON for one 30 ms mic frame.
- **Items are copied from the ringbuffer to a stack-local buffer
  before retry.** This frees the ringbuffer slot so the producer
  isn't blocked while the sender retries. The retry holds the local
  copy in 1.3 KB of sender_task stack.
- **The producer never drops.** `convai_send_enqueue` tries non-
  blocking first; if the queue is full, it blocks with `portMAX_DELAY`.
  `mic_task` will pause capture if the link is truly dead. No frame
  is ever silently dropped. Block events are counted in
  `s_queue_block_count`.
- **The sender never gives up.** It retries the same item forever (or
  until `s_running` flips false during `convai_stop`). The library
  never sees a failing-send pattern, so it never declares the
  connection dead.

### Lifecycle hooks

| Event | Action |
|---|---|
| `convai_start` | Allocate 1 MB PSRAM storage, create static ringbuffer, spawn `sender_task` with PSRAM stack |
| `WEBSOCKET_EVENT_DISCONNECTED` | Drain queue (frames are for the dead session; replaying them against a reconnected session's fresh greeting would confuse the server). Reset `s_q_bytes_in/out`. |
| `WEBSOCKET_EVENT_CONNECTED` (reconnect) | No action — queue is already empty from disconnect drain. Sender will resume on the next `mic_task` enqueue. |
| `convai_stop` | Set `s_running=false`, wait ~600 ms for `sender_task` to exit its 500 ms ringbuffer-receive poll, then `vRingbufferDelete` + free storage. |

### What gets queued vs. direct

| Send | Queued? | Why |
|---|---|---|
| Mic frame (`user_audio_chunk`) | **Yes** | High volume (33/s during PTT); the only realistic source of fail-fast |
| Silence pad (post-PTT) | **Yes** | Same path as mic frames; same cadence |
| `pong` reply | **No** (direct) | Infrequent (1-2/s) and intentionally skipped during PTT — would never drive fail-fast |
| `user_activity` keepalive | **No** (direct) | 1/60 s, too infrequent to matter |
| `conversation_initiation_client_data` | **No** (direct) | One-shot at WS connect; must succeed before any mic frame is meaningful |
| `user_message` (NFC custom phrase) | **No** (direct) | One-shot per tag scan; its turn-end silence pad that follows IS queued. See "Text injection" below |

---

## Pong handling (and why we skip during PTT)

Pings arrive every ~1–2 s from the server. The server expects a pong
reply in some bounded window.

Our pong reply is sent **inline from `process_message`**, *not* from
the sender task. The send is direct, bypassing the queue. This means
both the sender task (during PTT) and the pong handler can simultaneously
want the WS lib's internal mutex.

If `mic_task` is busy enqueueing frames every 30 ms and `sender_task`
is busy draining them, the lib's send mutex is in heavy use. A pong
reply waiting on that mutex for > 50 ms results in
`Could not lock ws-client within 50 timeout` from the lib. The pong
send then returns ≤ 0 — a dropped pong — and the server will
eventually time us out.

**Solution: skip pongs during PTT.** In the ping handler, if
`s_ptt_held == true`, we just count the ping in
`s_pongs_skipped_ptt` and return without replying. Skipped pongs
during PTT are *not* a server-side failure — the legacy Pi does the
same thing (cancels its pong task on PTT press) and conversations
remain stable.

Once PTT releases, the next ping is replied to normally. The skip is
a transient, by-design behaviour. The heartbeat's `pp_delta` metric
explicitly subtracts `skip_ptt` so a healthy session reads
`pp_delta=0` even after long PTT presses.

If you see `dropped=N` rising (where dropped = pong sends that
returned ≤ 0 outside the skip path), the server *will* time us
out — that's a real problem to investigate.

---

## `user_activity` keepalive (60 s)

The `inactivity_timeout=120` URL parameter tells the server "drop me
if you haven't seen client traffic in 120 s." During long agent turns
or idle periods we may not send anything for that long, so
`heartbeat_task` proactively sends `{"type":"user_activity"}` every
60 s while:

- the WS is up, AND
- PTT isn't currently held (mic_task owns the WS lock)

This matches `maintain_pong` in the legacy Pi.

The send is direct (bypasses the queue) because once per minute can't
plausibly drive fail-fast.

---

## End-of-turn detection (the `agent_response`-aware playback)

The playback task can't use a simple "no audio for 250 ms → MUTED"
heuristic because ElevenLabs delivers audio in chunks with multi-second
gaps mid-turn (TTS pause while LLM streams next sentence). On the first
agent message of a session this is especially long (LLM cold start).

The correct end-of-turn signal is the `agent_response` message — the
server emits it along with the LAST audio chunk of a turn. Set
`s_agent_done_signal = true` when received.

`playback_task` decides what to do when the PCM ring goes empty:

| State | Action |
|---|---|
| `agent_response` received AND ring empty | Transition to `ORB_MUTED` |
| `agent_response` NOT received AND ring empty | Stay on `ORB_AGENT` — more audio is coming |
| `agent_response` NOT received AND no audio bytes have arrived for `PLAYBACK_NO_AUDIO_SAFETY_MS` (10 s) | Safety fallback: log a warning and force `ORB_MUTED`. Prevents permanent lock-on if the server vanishes mid-turn. |

The safety timer measures **time since the last received audio byte**
(updated in `handle_audio_event`), not time since the ring drained.
This way a real slow-network turn with multi-second inter-chunk gaps
keeps the timer fresh as long as bytes keep arriving on the wire.

---

## PTT button gate during AGENT speech

Once `playback_task` is actively playing agent audio (`s_agent_speaking
= true`), `mic_task` ignores PTT presses entirely. Rationale: the
previous "interrupt the agent by pressing PTT" behaviour was confusing
(server received user audio mid-agent and processed both as one merged
turn). Now the user waits for the agent to finish, then talks. Simpler
mental model, no merged transcripts, no interruption events.

`mic_task`'s PTT rising edge does:

```c
if (s_agent_speaking) {
    ESP_LOGI(TAG, "PTT ignored — agent is speaking");
    while (s_ptt_held && s_running) vTaskDelay(20);
    continue;
}
```

So a press during agent speech is swallowed entirely, and the user
must release + re-press once the agent is done. Defense in depth.

---

## PTT timing constants (the values we actually run)

All in `convai.c` near the `mic_task` definition.

| Constant | Value | Source / why |
|---|---|---|
| Frame size | 30 ms / 480 samples / 960 bytes | Matches the legacy Pi byte-for-byte so server-side framing is identical |
| `PTT_POWER_RAIL_WAIT_MS` | 150 | Stabilisation after GPIO change before opening mic |
| `PTT_PRESS_MIN_MS` | 1000 | Press < 1 s → silent revert (no turn end sent) |
| `PTT_TAIL_SILENCE_FRAMES` | 43 (~1290 ms) | Must exceed the server's VAD silence threshold + jitter margin |
| `PTT_SHORT_TURN_MIN_MS` | 800 | Real audio < 800 ms → skip waiting for response, but DO send the pad to close the turn server-side |
| `PTT_STALE_TURN_WARN_MS` | 8000 | Watchdog: warn if PTT closed > 8 s ago and nothing but pings has arrived |

The pad / VAD relationship matters: the initiation message also pins
`turn_detection.silence_duration_ms = 800` on the server side, so our
1290 ms pad gives ~490 ms margin over the threshold. Don't shorten
the pad without lowering the server threshold to match.

---

## Text injection — `convai_send_user_message` (NFC custom-phrase tags)

A custom-phrase NFC tag (BEHAVIOR.md §6) drops a line of text into the **live**
conversation as if the user had spoken it — no session restart. `nfc.c::handle_uid`
calls `convai_send_user_message(phrase)`; the agent answers and the session
continues. This is the one place we feed the agent user input that isn't mic audio.

**It is a two-part send, and both parts are mandatory:**

1. **The text event** — `{"type":"user_message","text":"<phrase>"}`, built with cJSON
   (so quotes / UTF-8 are escaped) and **direct-sent** (not via the mic queue), same
   class as `pong` / `user_activity` / `send_initiation`. One-shot, too infrequent to
   drive the lib's fail-fast.
2. **A turn-end pad** — immediately after the text we push `PTT_TAIL_SILENCE_FRAMES`
   of zero audio through the queue (the same pad a PTT release sends).

**Why part 2 is not optional (the bug we hit and fixed):** the session runs
**server-side VAD turn detection on the audio stream** (`turn_detection.silence_duration_ms`).
The server decides "user is done → agent's turn" by hearing audio go silent. A
text-only `user_message` carries no audio, so the server registers the text but never
sees an end-of-turn — the agent stays silent forever and the UI sticks on `ORB_LOADING`.
The silence pad is the **ESP32 realization of the Pi's `force_turn_end`** (BEHAVIOR.md
§3.5). Unlike the Pi — which only force-ended when a tag was scanned *mid-press* — we
fire it unconditionally, because NFC only scans while PTT is idle (§6.4 gating) so there
is otherwise no audio turn boundary at all.

**Healthy trace** (from a verified run):
```
custom phrase -> inject "tell me about your backpack"
user_message injected (rc=60)
sender: sent #33 (item=1303B ...)     ← the silence pad going out
rx: type=audio ... (×N)               ← agent responds
agent: This is my bag, yeah. ...
```
If you instead see `user_message injected` followed by **only pings** (`rx_2s` stays at
ping size, no `audio`/`agent_response`), the turn-end didn't land — check that the pad is
still being sent.

**Guards / ownership:** `convai_send_user_message` returns false (no-op) if the WS is
down, no session is running, or `s_ptt_held` is true. The PTT guard matters because the
pad reuses `s_mic_pcm` via `send_silence_frames`, which `mic_task` also owns during a
press; the §6.4 NFC gate (scan only in `ORB_MUTED`) already guarantees PTT is idle, but
the guard is belt-and-suspenders. The call blocks ~1.29 s (the pad is paced at 30 ms/frame)
in `nfc_task`, which is fine — NFC polling is suppressed during `ORB_LOADING` anyway.

---

## Heartbeat — the diagnostic surface

Every 2 s, `heartbeat_task` emits one long line. While `s_log_verbose`
is true (currently default-on during the queue rollout) it looks like:

```
hb: ws=up rx_age=4ms tx_age=989ms rx_2s=73662B last=agent_chat_response_part
    lvgl_d=6134 ptt=0 heap_int=11K psram=3196K conn#1 disc#0
    pings=6 pongs=3 dropped=0 skip_ptt=3 pp_delta=0
    q_depth=0B q_block=0 q_retry=0
```

Field-by-field reference lives in [README.md → "Heartbeat line"](README.md).
What's worth knowing for triage:

- **`ws=DOWN`** ever → the lib disconnected; expect a `conn#` bump shortly
- **`tx_age` climbing past 2 s with no PTT** → user_activity keepalive may have failed; check for `user_activity keepalive send failed` log
- **`heap_int` < 4 KB** → fragmentation risk; the next WS task spawn could fail. Cross-check the heap probe at next reconnect.
- **`dropped` > 0** → pong send failures *outside* the PTT skip path; server will time us out
- **`pp_delta` ≠ 0** → pongs were dropped unintentionally (after subtracting `skip_ptt`)
- **`q_depth` > 0 between PTT presses** → sender is behind; not necessarily bad, but watch `q_retry` to see if it's recovering naturally
- **`q_block` rising** → producer blocked because queue is full; link is genuinely bad (we don't drop, so we slow)
- **`q_retry` rising** → `send_text` is returning ≤ 0; check for `sender: send_text returned X` warning lines for the actual return code

Plus the spot-check line:

```
sender: sent #N (item=NB, last_fail_streak=K)
```

every 33 successful sends. Confirms the sender task is alive. `last_fail_streak`
is the count of failures we had before the last successful send recovered.

The heap probe at WS-start is also load-bearing:

```
start: heap_int=N largest_int=M psram=K before client_start
```

If `largest_int` ever drops below ~6500, the WS lib's task spawn is at
risk and we need to revisit fragmentation strategy.

---

## Failure modes (do not re-try)

| Tried | Result |
|---|---|
| `mic_task` calls `esp_websocket_client_send_text` directly | The original architecture — `transport_poll_write(0)` killed conversations every minute or two. The whole queue exists to never go back here. |
| WS task stack at 8192 | By the time `convai_start` runs, internal heap's largest contiguous block is ~7.5 KB. Spawn fails silently. 6144 fits. |
| `sender_task` stack in internal RAM | Internal heap is fragmented to small blocks by the time the 4 KB stack allocation runs. Spawn fails silently and no `sender_task started` log appears. Must use `xTaskCreatePinnedToCoreWithCaps(MALLOC_CAP_SPIRAM)`. |
| Replying to pongs from `process_message` while PTT is held | WS lib mutex contention with mic frame sends → pong send timeouts → server times us out. Skip pongs while `s_ptt_held` instead. |
| Auto-reconnect re-running `send_initiation` mid-conversation | Server treats it as a new session and re-greets. Symptom is "conversation restarts mid-PTT." The queue prevents the disconnect that triggers this in the first place; if you ever want to handle the ECONNRESET case differently, do not silently re-init. |
| Direct send of `user_audio_chunk` from anywhere | Same as the first row. The architectural invariant is: only `sender_task` sends mic frames. |
| `network_timeout_ms` bumps to 30 s | Doesn't help — the lib's fail-fast is per-write, not cumulative. Even at 30 s, a single write that's congested for the full timeout will still trigger the disconnect chain. |
| Inactivity timeout 600 s instead of 120 s | Tested; doesn't matter because we send `user_activity` every 60 s. The 120 s window is plenty as long as the keepalive lands. |

---

## Diagnostics workflow

When things go wrong, in order:

1. **First check `q_depth`, `q_block`, `q_retry` in the heartbeat.**
   - If all 0 and the symptom is "no agent response": the audio reached
     the server fine. Look at server-side or `last=` to see what
     ElevenLabs is doing.
   - If `q_depth` is growing without `q_retry` growing: the sender is
     processing but slow — usually means very congested upstream.
   - If `q_retry` is growing: the WS lib's send is failing. Read the
     `sender: send_text returned X` warning for the actual return code.
2. **Look for `sender_task started` at session start.** If absent, the
   sender didn't spawn — usually a PSRAM allocation failure. Check
   `start: heap_int=N largest_int=M psram=K`.
3. **Look for `ws disconnected (#N)` lines.** They include
   `err_type / esp_tls_err / tls_stack / sock_errno`. `sock_errno=104`
   is ECONNRESET — server or middlebox closed us, no client-side fix
   possible. `tls_stack` non-zero is an mbedTLS error and likely
   reconfigurable.
4. **`TURN STALE: Nms since PTT close`** = PTT released and 8+ s
   passed without any non-ping rx. Either the server didn't see
   end-of-turn (check pad timing) or the LLM cold-start is taking long
   (first turn only, common).
5. If everything looks healthy and the orb is stuck, check `lvgl_d`
   in the heartbeat. If it's 0 over a 2 s window, the LVGL task is
   frozen — different bug, see SCREEN.md.

---

## Deferred: resume the conversation on reconnect (`conversation_id`)

**Status: not implemented — design note for later.**

**Problem.** A WiFi blip / `ECONNRESET` / STA reassoc kills the stateful WSS. On
reconnect the lib re-runs `send_initiation`, which the server treats as a brand-new
conversation → the agent re-greets and all prior context (who the user is, what was
discussed) is gone. Seen live: WiFi dropped, the orb pulled a fresh DHCP lease, and
the conversation restarted from the greeting.

**Idea.** ElevenLabs assigns a `conversation_id`, delivered in the
`conversation_initiation_metadata` event we currently only **log** (`convai.c`
`process_message`, the `conversation_initiation_metadata` branch — we don't parse it).
If the API supports reattaching to an existing conversation, capture that id and, on a
*transport reconnect* (not a deliberate restart), resume it instead of re-greeting.

**To pick up, in order:**
1. **Verify the API first — load-bearing unknown.** Confirm whether ElevenLabs ConvAI
   supports resuming a conversation over a *new* WSS, and the exact mechanism (likely a
   query param e.g. `&conversation_id=<id>` on the WS URL, or a field inside
   `conversation_initiation_client_data`). If it's unsupported, the rest is moot — don't
   build on an assumption. Check the ElevenLabs agents/WSS docs.
2. **Capture the id.** In the `conversation_initiation_metadata` branch, pull
   `conversation_initiation_metadata_event.conversation_id` into a static
   (`s_conversation_id`).
3. **Distinguish reconnect from restart.** Only resume on a transport-level reconnect
   (`WEBSOCKET_EVENT_DISCONNECTED` → auto-reconnect). A deliberate NFC `AGENT_START`/`TEST`
   (`convai_stop()` → `convai_start()`) must still start **fresh** — clear
   `s_conversation_id` in `convai_stop()`.
4. **Resume in `send_initiation`.** If `s_conversation_id` is set, send the resume variant
   **and** suppress the greeting (this is the same machinery as the still-pending
   `did_init` / `SUPPRESS_GREETING`, BEHAVIOR.md §2.2). Otherwise send the normal
   fresh-greeting init.
5. **Always have a fallback.** The server times conversations out (our URL sets
   `inactivity_timeout=120`). If resume fails (expired/unknown id), fall back to a fresh
   `send_initiation` — never hang waiting on a dead id.

**Related:** `did_init` + `SUPPRESS_GREETING` (BEHAVIOR.md §2.2, PROGRESS.md pending) —
both are "a reconnect within a session shouldn't re-greet."

## Things not in this doc that might bite you

- **The legacy Pi** ([futuresGarden/main.py](futuresGarden/main.py))
  uses `inactivity_timeout=600` and `websockets` async backpressure.
  Worth re-reading if a new failure mode appears.
- **The send queue does not solve ECONNRESET or WiFi reassoc.** Those
  are real disconnects; the WS dies and on reconnect we re-greet.
  Resuming the conversation instead of re-greeting is scoped in
  **"Deferred: resume the conversation on reconnect (`conversation_id`)"**
  above. Detecting the case and showing the user something is the
  lighter-weight alternative if full resume isn't supported.
- **HW AES is off.** TLS throughput is ~half what it could be. If
  you ever need to crank network volume (multiple simultaneous WSS,
  larger audio rates), HW AES + careful DMA placement is the lever.
- **WS task stack at 6144 is tight.** If the WS handler stack ever
  overflows (deeper cJSON recursion than expected, larger audio
  chunk), you'll see `websocket_client stack overflow` and a crash.
  At that point either grow the stack (and solve the contiguous-block
  problem somehow) or refactor the handler to use less stack.
