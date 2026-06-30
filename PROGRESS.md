# The Orb — ESP32-S3 build progress

Working base: this repo (`futuresGarden_ESP32`, IDF project name `ESP32-S3-Touch-LCD-2.8C-Test`).
Board, display (LVGL **8.4** — see the migration note at the bottom; an earlier LVGL 9 attempt
was reverted), WiFi, I2C, NFC, I2S audio, boot-time GitHub OTA, and the ElevenLabs ConvAI
WebSocket all confirmed working end-to-end. Push-to-talk conversation with the agent works.
Volume + agent are hot-reloaded from Supabase on every tag scan.
---

## Phase 0 — Clean up the example ✅

- [x] In `main.c`: remove `QMI8658_Loop()` and `RTC_Loop()` from `Driver_Loop`. Keep the task for `BAT_Get_Volts()`.
- [x] Remove `PCF85063_Init()` and `QMI8658_Init()` from `Driver_Init()`. **Files deleted entirely** (QMI8658/, PCF85063/, Buzzer/ directories removed; CMakeLists.txt + TCA9554PWR.{c,h} + LVGL_Example.{c,h} updated to match).
- [x] Remove `Touch_Init()` call in `app_main` — GT911 not needed. **Touch removed entirely**: `Touch_Driver/` deleted, GT911 indev + `example_touchpad_read` stripped from `LVGL_Driver.{c,h}`.
- [x] Replace `Lvgl_Example1()` with the Orb state machine (see Phase 1).
- [x] `main/secrets.h` is scaffolded with WiFi creds + ElevenLabs API key + Supabase URL.

---

## Phase 1 — Orb UI ✅

LVGL **8.4**, display driver registered **directly** (no `esp_lvgl_port`). Tear-free via
Travis's bounce-buffer + vsync-semaphore + single-FB config — see [SCREEN.md](SCREEN.md).
(An LVGL 9 + `esp_lvgl_port` build was tried and reverted; the stale notes are at the bottom
of this file, kept only for history. `CONFIG_LCD_RGB_RESTART_IN_VSYNC` is **not** used.)

**States**: BOOT → WIFI → CONFIG → (LOADING / USER_TALK / AGENT / MUTED). Plus
CHECK_UPD / UPDATING (OTA), WIFI_FAIL, NFC, ERROR, LOW_BAT, and a defined-but-unused
SPLASH. Full table with colours/labels/motion is authoritative in [README.md](README.md).

**Layout** (480×480 round, centre = 240,240): agent name + state label both live **inside**
the central circle (name Montserrat 24 at y-offset −22, state label Montserrat 26 at +18);
the battery readout sits at **TOP_MID** (Montserrat 20). The earlier bottom-anchored layout
(agent name y=90, battery y=390) is gone.

**State colours / labels** (in `main/LVGL_UI/orb_ui.c::STATE_STYLE`):

| State | Colour | Label |
|---|---|---|
| `ORB_BOOT` | `#404040` | "Booting" |
| `ORB_WIFI` | `#404040` | "Connecting WiFi" |
| `ORB_CONFIG` | `#404040` | "Fetching config" |
| `ORB_CHECK_UPD` | `#404040` | "Checking for updates" |
| `ORB_UPDATING` | `#404040` | "Updating" |
| `ORB_WIFI_FAIL` | `#C0392B` red | "No WiFi. Check router & restart" |
| `ORB_SPLASH` | `#2A2A2A` | "Ready" (defined, never set at runtime) |
| `ORB_LOADING` | `#C47A0C` amber | "Loading" |
| `ORB_USER_TALK` | `#27AE60` green | "Listening" |
| `ORB_AGENT` | `#1A7DC4` blue | "Agent speaking" |
| `ORB_MUTED` | `#2A2A2A` | "Press to talk" |
| `ORB_NFC` | `#7B2FA8` purple | "NFC Scanned" |
| `ORB_ERROR` | `#C0392B` red | "ERROR Restart the Orb." |
| `ORB_LOW_BAT` | `#C0392B` red | "Low battery" |

**Threading rule:** state setters write atomics; a 50 ms `lv_timer_create()` callback
on the LVGL task renders. No mutex needed. Agent name + battery voltage use the
same pattern.

- [x] `orb_ui_init()` — widgets + render-tick timer
- [x] `orb_ui_set_state(OrbState s)` — atomic, applied on next tick
- [x] `orb_ui_set_agent_name(const char *)` — same pattern
- [x] `orb_ui_set_battery(float volts)` — same pattern

### Two-state runtime model (per BEHAVIOR.md §1)

| Logical state | Convai | Orb UI states used |
|---|---|---|
| `splash_idle` | WSS closed | `ORB_SPLASH` |
| `running_agent` | WSS open | `ORB_LOADING` (between turn boundaries), `ORB_AGENT` (agent talking), `ORB_USER_TALK` (PTT held), `ORB_MUTED` (between turns inside session) |

The convai module owns the state transitions. NFC handler sets `ORB_LOADING` on tag scan,
then `convai_start()` drives the rest.

---

## Phase 2 — WebSocket + ElevenLabs ✅

`main/Convai/convai.{c,h}` — full ConvAI client. Persistent WSS across many user turns.

### What works

- [x] **WSS connect** to `wss://api.elevenlabs.io/v1/convai/conversation?agent_id=<id>&inactivity_timeout=120`
- [x] **TLS**: GTS Root R1 cert pinned via `EMBED_TXTFILES "certs/gts_root_r1.pem"`. The built-in mbedTLS CA bundle didn't match, so we pin the cert directly (same pattern as `certs/gts_root_r4.pem` for Supabase).
- [x] **Auth**: `xi-api-key: <EL_API_KEY>` header (from `secrets.h`)
- [x] **Initiation**: `conversation_initiation_client_data` with PCM 16 kHz on both directions
- [x] **WS frame reassembly** (96 KB → bumped to 2 MB PSRAM buffer; ElevenLabs sends single audio events of 200+ KB for short greetings)
- [x] **Chunked base64 decode** (1 KB / 768 B at a time) with `portMAX_DELAY` push to PCM ring → small fixed memory regardless of audio length
- [x] **PCM ring** (128 KB PSRAM `xStreamBufferCreateStatic`) — smoothing buffer between WS receive and speaker
- [x] **Playback task** (core 1, prio 5, 4 KB) — drains ring 1024 samples at a time into `i2s_audio_write`
- [x] **Ping → pong** with matching `event_id` (the only required keepalive per ElevenLabs docs)
- [x] **`interruption` event** — flushes the PCM ring
- [x] **`agent_response` / `user_transcript`** logged
- [x] **Persistent WSS across many user turns** — opens once on tag scan, stays open until `convai_stop` (re-scan triggers stop + restart)
- [x] **PTT** — see Phase 4
- [x] **WSS stack**: `task_stack = 6144` (default 4 KB overflows cJSON + mbedTLS + decode chunk; was 8192 but internal-heap fragmentation by the time `convai_start` runs means the largest contiguous block is ~7.5 KB, so 8 KB fails to allocate — see CLAUDE.md / CONVAI.md)

### NFC integration (tag table — implemented)

The real BEHAVIOR.md §6 tag table is live. The UID→phrase map is **downloaded from
GitHub each session** (`NFC_TAGS_URL` in `secrets.h`, raw file on the public OTA repo,
fetched via the cert bundle in `config_fetch.c::orb_nfc_tags_fetch()` into a 32 KB PSRAM
buffer). No embed, no NVS cache — the orb needs internet to converse anyway. Parsed into
`cJSON *s_tags`, owned solely by `nfc_task` (warmed before the poll loop, lazily reloaded
on first scan, reloaded on every AGENT_START/TEST).

`handle_uid` normalizes the UID to colon-hex (`04:38:17:9A:CB:2A:81`, matching the JSON
keys) and **looks it up before any UI/tone**, then dispatches:

- `AGENT_START` / `TEST` → reload tags + `orb_refresh_config()` → `convai_stop()` →
  `convai_start()`. (No splash/temp-WSS state in this firmware, so both mean "reload +
  restart".)
- Any other mapped phrase, session running → **inject** `{"type":"user_message","text":
  "<phrase>"}` into the live WSS via `convai_send_user_message()` — no restart.
- Mapped phrase, no session (rare) → restart; phrase dropped.
- **Unmapped UID → ignored** (no tone, no state change).

`orb_refresh_config()` re-runs the Supabase GET and applies new `agent_id`, `agent_name`,
and `volume` (→ `i2s_audio_set_volume_pct(cfg.volume * 10)`).

**NFC scanning is gated to the idle window** — `orb_ui_set_state()` calls
`NFC_Set_Polling(s == ORB_MUTED)`, so reads only happen between turns and are suppressed
during `ORB_USER_TALK` / `ORB_LOADING` / `ORB_AGENT`. This is stricter than BEHAVIOR.md
§6.4 (which keeps NFC live during the agent response): on this build it's off while the
agent talks too.

### Still pending (Stage 4–5 of the original plan)

- [ ] **Init JSON shape per BEHAVIOR.md §2.1**: nest under `agent / tts / asr` keys with `language: "en"` and conditional `tts.volume`. Today we send a flatter shape that the server tolerates.
- [x] **NFC tag table**: AGENT_START / TEST / custom-phrase inject (`{"type":"user_message","text":"..."}`). Tag library **downloaded from GitHub each session** (not URL+NVS-cached as originally specced — no offline fallback by design).
- [ ] **`did_init` + SUPPRESS_GREETING on reconnect** (BEHAVIOR.md §2.2)
- [ ] **First-turn barge-in** allowing PTT mid-greeting
- [x] **NFC polling gated to the idle window** (`ORB_MUTED` only) — stricter than BEHAVIOR.md §6.4; suppressed during user-turn capture **and** agent response.
- [ ] **TEST tag hot-reload via temp WSS** (BEHAVIOR.md §7) — collapsed into "reload + restart" since there's no splash state.

---

## Phase 3 — NFC (PN532, J8) ✅

PN532 NFC reader on **J8** I2C header, address `0x24`. DIP switches: SW1=ON, SW2=OFF.

- [x] PN532 I2C init (5-attempt retry, 30 s long-cycle recovery)
- [x] Polling task at 100 ms (was 500 ms — tightened to reduce scan latency)
- [x] UID normalization + 1.5 s same-UID debounce
- [x] PN532 not-responding error surfaced via the `errors=N` counter (no LCD scene yet)
- [x] On tag scan: tag-table lookup → dispatch (AGENT_START/TEST restart · custom-phrase inject · unmapped ignore)
- [x] Polling gated to `ORB_MUTED` only (off during PTT capture and agent response)
- [x] Poll heartbeat log silenced once polling was stable — uncomment in `nfc.c` if it regresses

---

## Phase 4 — I2S audio ✅

`main/Audio/i2s_audio.{c,h}` — duplex MAX98357A speaker + INMP441 mic.

### Hardware (see [j9-audio-wiring.md](j9-audio-wiring.md))

- **MAX98357A** speaker amp on GPIO 19 (DIN), 3V3 powered, gain option A
- **INMP441** mic on **GPIO 20** (was GPIO 0; strap-pin pull-up masked the open-drain output). L/R tied to GND.
- Shared BCLK (GPIO 43) + WS (GPIO 44).

### Driver design — split I2S

Two separate I2S peripherals:
- **I2S_NUM_0** TX master — drives BCLK / WS / DOUT to MAX98357A. Always running.
- **I2S_NUM_1** RX slave — reads BCLK / WS as inputs (from the same physical pins via GPIO matrix), reads DIN from INMP441.

Single-peripheral duplex was unstable (clocks gated when TX disabled → mic produced
nothing). Split avoids the duplex-clock-gating gotcha entirely.

### Format

- 16 kHz, 32-bit slot (INMP441 needs 32 BCLK / slot or it never drives data; MAX98357A is happy with 16-bit data MSB-aligned in a 32-bit slot)
- `ws_width = 32`, `left_align = true`, Philips standard
- TX: int16 → `<< 16` to int32 widening
- RX: int32 (top 24 bits = INMP441 sample) → `>> 15` to int16 (1 bit software gain)

### API

- [x] `i2s_audio_init()` — duplex up at boot (before WiFi). USB-CDC + UART bridge die here.
- [x] `i2s_audio_beep(freq_hz, duration_ms)` — sine with 5 ms envelope to avoid pops
- [x] `i2s_audio_tone_sequence(tones, count)` — back-to-back tones
- [x] `i2s_audio_write(samples, count)` — stream samples for live playback
- [x] `i2s_audio_play(samples, count)` — one-shot, drains then quietens DMA
- [x] `i2s_audio_record(out, count)` — one-shot fixed-length capture with post-processing (settle / fade / DC removal / despike)
- [x] `i2s_audio_record_start() / _chunk() / _stop()` — streaming capture for PTT
- [x] `i2s_audio_set_volume_pct(pct)` — runtime software TX volume (called from `orb_refresh_config`)

### Amp mute trick (`dout_mute` / `dout_unmute`)

I2S TX channel can't be disabled during PTT (mic shares its clocks), but we don't
want speaker noise while the user is talking. The DOUT pin (GPIO 19) is dynamically
reassigned via the GPIO matrix:
- `dout_mute()`: route DOUT to GPIO output low → MAX98357A sees steady 0 V → auto-mute after ~25 ms
- `dout_unmute()`: route DOUT back to I2S TX signal

Done transparently inside `i2s_audio_record_start` / `i2s_audio_record_stop` and
`tx_audible` (called by all play paths).

---

## Phase 5 — PTT (push-to-talk) ✅

Button on GPIO 0 (button to GND). `main/Button/button.{c,h}` exposes
`button_init(button_state_cb_t cb)` — callback fires on press AND release with the
new state, debounced 50 ms, run on a 6 KB dedicated task.

⚠️ **GPIO 0 boot caveat**: holding the button at power-up puts the chip in ROM
download mode. Don't hold it during reset.

### PTT timing (BEHAVIOR.md §3.1, with a project-decided pad reduction)

| Constant | Value | Source |
|---|---|---|
| `DEBOUNCE_MS` | 50 ms | spec §3.1 |
| `PTT_POWER_RAIL_WAIT_MS` | 150 ms | spec §3.3 step 2 |
| `PTT_PRESS_MIN_MS` | 1000 ms (silent-revert threshold) | spec §3.4 |
| Frame | 30 ms / 480 samples / 960 bytes | spec §3.1 |
| `PTT_TAIL_SILENCE_FRAMES` | 43 (≈ 1290 ms pad) | **project decision** — spec is 50 (1500 ms); 43 keeps ~490 ms margin over the server VAD threshold (`silence_duration_ms=800`). An earlier 27 (810 ms) sat on the threshold and merged consecutive presses — see CLAUDE.md. |
| `PTT_SHORT_TURN_MIN_MS` | 800 ms | spec §2.4 |

### Behavior (in `main/Convai/convai.c::mic_task`)

1. **Press detected** → wait 150 ms power-rail stabilization. If released within the wait, abort silently (mic never opens).
2. **Mic opens**: `ORB_USER_TALK` (green), start streaming 30 ms PCM frames as `{"user_audio_chunk":"<base64>"}`.
3. **Release**:
   - **`press_duration < 1000 ms`** → silent revert. No pad. No turn end. Back to `ORB_MUTED`.
   - **`real_audio_ms < 800 ms`** (press ≥ 1000 ms but only N frames actually streamed — RX hiccup) → short-turn skip. Same outcome.
   - **Otherwise**: `ORB_LOADING` (yellow), send 43 × 30 ms silence frames so server VAD trips end-of-utterance, then idle.

After the agent responds, `playback_task` returns the orb to `ORB_MUTED` when the PCM ring
drains **and** the `agent_response` end-of-turn signal has been received (a 10 s no-audio
safety fallback covers a server that vanishes mid-turn). See [CONVAI.md](CONVAI.md) →
"End-of-turn detection".

### Keepalive

Two mechanisms (both ported from the legacy Pi):

- **`ping → pong`** on every server ping, with matching `event_id` — sent inline, but
  **skipped while PTT is held** (mic frames own the WS lock; a contended pong would
  50 ms-timeout). Skipped pongs are counted and resume on release.
- **Periodic `user_activity` every 60 s** while the WS is up and PTT is idle — proactive
  keepalive so the server's ping cadence can't drift past our cutoff.

`inactivity_timeout=120` URL parameter extends the server-side "no client traffic" cutoff.
See [CONVAI.md](CONVAI.md) for the full pong-skip and keepalive rationale.

---

## LVGL 9 migration notes — ⚠️ SUPERSEDED / REVERTED

> **Do not follow this section.** The LVGL 9 + `esp_lvgl_port` migration described below
> was **tried and reverted** — it tore on this panel in every config combination. The
> shipping stack is **LVGL 8.4 with the display driver registered directly** (no
> `esp_lvgl_port`), per [SCREEN.md](SCREEN.md), which is the source of truth. Both
> `CONFIG_EXAMPLE_DOUBLE_FB` and `CONFIG_LCD_RGB_RESTART_IN_VSYNC` are **off**. The notes
> are kept only as a record of what not to retry.

The reverted v9 attempt used `esp_lvgl_port` with `num_fbs = 2`
(`CONFIG_EXAMPLE_DOUBLE_FB=y`), `direct_mode = true`, and
`CONFIG_LCD_RGB_RESTART_IN_VSYNC=y`. v8↔v9 API differences that mattered:
`lv_scr_act` ↔ `lv_screen_active`, `lv_obj_clear_flag` ↔ `lv_obj_remove_flag`
(the shipping 8.4 code uses the `lv_scr_act` / `lv_obj_clear_flag` forms), and
`lv_obj_remove_style_all(scr)` is still required either way to defeat default theme
padding. See the failed-approaches table in [SCREEN.md](SCREEN.md).

---

## Build & flash

```
idf.py reconfigure         # only when CMakeLists.txt / sdkconfig.defaults / idf_component.yml changed
idf.py build
idf.py flash               # incremental
idf.py monitor             # ONLY useful before audio init runs (~5–8 s after boot)
```

**Do not run `idf.py set-target` again** — it wipes sdkconfig.

USB-CDC + UART bridge both die when `i2s_audio_init()` runs. Diagnostics after that
point go over UDP — see Diagnostics in [README.md](README.md). Mac listener:

```
nc -ul 6666
```

If your AP blocks broadcast (or you're on a routed segment), set
`LOG_SINK_TARGET_IP` to the listener's IP in `main/Wireless/log_sink.c` (currently
hardcoded to `192.168.1.27`).

---

## Status summary

| Phase | Status |
|---|---|
| Phase 0 — example cleanup | ✅ done |
| Phase 1 — Orb UI / LVGL 8.4 / state machine | ✅ done |
| Phase 2 — WiFi + SNTP + Supabase config | ✅ done |
| Phase 2b — ElevenLabs WSS + persistent session | ✅ done (init JSON shape pending) |
| Phase 2c — App-level send queue (1 MB PSRAM + sender_task) | ✅ done — see [CONVAI.md](CONVAI.md) |
| Phase 3 — NFC PN532 | ✅ done |
| Phase 4 — I2S audio (TX + RX) | ✅ done |
| Phase 5 — PTT | ✅ done |
| Phase 5b — Boot-time GitHub OTA + rollback safety | ✅ done — see [DEPLOYMENT.md](DEPLOYMENT.md) / `main/OTA/ota.c` |
| Phase 6 — NFC tag table (AGENT_START / TEST / custom phrase) | ✅ done — GitHub-downloaded table, live-session phrase inject, polling gated to idle. Remaining: TEST-tag temp-WSS greeting (collapsed into restart; no splash state) |
| Phase 7 — Battery monitoring per BEHAVIOR.md §9 | partial — GPIO4 ADC + 2 s moving average + load-anchored % gauge wired; idle-gating N/A (no idle state); current-sense N/A (no INA219). See [BATTERY.md](BATTERY.md). Remaining: real brown-out `EMPTY` anchor, harness-resistance fix |
| Phase 8 — Telemetry POST | not started |
| Phase 9 — Interface polish (this is what we're moving to next) | next |

---

## What's proven end-to-end

- Boot → WiFi → SNTP → OTA check → Supabase config → LOADING → agent greeting (auto-start, no idle SPLASH)
- Tag scan → re-fetch config → WSS open → agent greeting plays through speaker
- Hold button → mic streams 30 ms PCM chunks to server
- Release → silence pad → agent transcribes + responds → response plays
- Loop for many turns within the same WSS
- Re-scan tag → fresh config fetch (volume / agent change applies live) → restart conversation
- LCD state transitions match what's happening (BOOT / WIFI / CONFIG / LOADING / USER_TALK / AGENT / MUTED; OTA shows CHECK_UPD / UPDATING)
- UDP logs survive bursts; on-screen state survives WiFi blips
