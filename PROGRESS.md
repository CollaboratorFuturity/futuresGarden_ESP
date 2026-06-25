# The Orb — ESP32-S3 build progress

Working base: this folder (`ESP32-S3-Touch-LCD-2.8C-Test`). Board, display, LVGL 9, WiFi,
I2C, NFC, I2S audio, and ElevenLabs ConvAI WebSocket all confirmed working end-to-end.
Push-to-talk conversation with the agent works. Volume + agent are hot-reloaded from
Supabase on every tag scan.
e
---

## Phase 0 — Clean up the example ✅

- [x] In `main.c`: remove `QMI8658_Loop()` and `RTC_Loop()` from `Driver_Loop`. Keep the task for `BAT_Get_Volts()`.
- [x] Remove `PCF85063_Init()` and `QMI8658_Init()` from `Driver_Init()`. **Files deleted entirely** (QMI8658/, PCF85063/, Buzzer/ directories removed; CMakeLists.txt + TCA9554PWR.{c,h} + LVGL_Example.{c,h} updated to match).
- [x] Remove `Touch_Init()` call in `app_main` — GT911 not needed. **Touch removed entirely**: `Touch_Driver/` deleted, GT911 indev + `example_touchpad_read` stripped from `LVGL_Driver.{c,h}`.
- [x] Replace `Lvgl_Example1()` with the Orb state machine (see Phase 1).
- [x] `main/secrets.h` is scaffolded with WiFi creds + ElevenLabs API key + Supabase URL.

---

## Phase 1 — Orb UI ✅

LVGL 9 via `esp_lvgl_port` (migrated from LVGL 8.2 — see "LVGL 9 migration notes" below).
RGB panel scan-out drift fixed with `CONFIG_LCD_RGB_RESTART_IN_VSYNC=y`.

**States**: BOOT → WIFI → CONFIG → SPLASH → (LOADING / USER_TALK / AGENT / MUTED / LOW_BAT)

**Layout** (480×480 round, inscribed circle, centre = 240,240):
- y=90: agent name label — Montserrat 20
- y=240: 200 px filled circle — colour + label change per state
- y=390: battery voltage label — Montserrat 16

**State colours / labels** (in `main/LVGL_UI/orb_ui.c::STATE_STYLE`):

| State | Colour | Label |
|---|---|---|
| `ORB_BOOT` | `#404040` | "Booting" |
| `ORB_WIFI` | `#404040` | "Connecting WiFi" |
| `ORB_CONFIG` | `#404040` | "Config" |
| `ORB_SPLASH` | `#2A2A2A` | "Ready" |
| `ORB_LOADING` | `#C47A0C` amber | "..." |
| `ORB_USER_TALK` | `#27AE60` green | "Listening" |
| `ORB_AGENT` | `#1A7DC4` blue | "Speaking" |
| `ORB_MUTED` | `#2A2A2A` | "Muted" |
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
- [x] **WSS stack**: bumped `task_stack` to 8192 (default 4 KB overflows cJSON + mbedTLS + decode chunk)

### NFC integration (test mode)

Per BEHAVIOR.md §6 the real tag table is AGENT_START / TEST / custom phrase. Today
we treat every scanned tag as AGENT_START:

- First scan in `splash_idle` → `orb_refresh_config()` → `convai_start(agent_id)`
- Re-scan in `running_agent` → `convai_stop()` → `orb_refresh_config()` → `convai_start()` (fresh agent / volume)

`orb_refresh_config()` re-runs the Supabase GET and applies new `agent_id`, `agent_name`,
and `volume` (→ `i2s_audio_set_volume_pct(cfg.volume * 10)`).

### Still pending (Stage 4–5 of the original plan)

- [ ] **Init JSON shape per BEHAVIOR.md §2.1**: nest under `agent / tts / asr` keys with `language: "en"` and conditional `tts.volume`. Today we send a flatter shape that the server tolerates.
- [ ] **NFC tag table**: AGENT_START / TEST (hot reload) / custom-phrase (inject `{"type":"user_message","text":"..."}`). Tag library fetched from URL, NVS cached.
- [ ] **`did_init` + SUPPRESS_GREETING on reconnect** (BEHAVIOR.md §2.2)
- [ ] **First-turn barge-in** allowing PTT mid-greeting
- [ ] **NFC polling disable during user-turn audio capture** (BEHAVIOR.md §6.4)
- [ ] **TEST tag hot-reload via temp WSS** (BEHAVIOR.md §7)

---

## Phase 3 — NFC (PN532, J8) ✅

PN532 NFC reader on **J8** I2C header, address `0x24`. DIP switches: SW1=ON, SW2=OFF.

- [x] PN532 I2C init (5-attempt retry, 30 s long-cycle recovery)
- [x] Polling task at 100 ms (was 500 ms — tightened to reduce scan latency)
- [x] UID normalization + 1.5 s same-UID debounce
- [x] PN532 not-responding error surfaced via the `errors=N` counter (no LCD scene yet)
- [x] On tag scan: refresh config from Supabase, start (or restart) convai
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
| `PTT_TAIL_SILENCE_FRAMES` | 27 (≈ 810 ms pad) | **project decision** — spec is 50 (1500 ms), we shortened it |
| `PTT_SHORT_TURN_MIN_MS` | 800 ms | spec §2.4 |

### Behavior (in `main/Convai/convai.c::mic_task`)

1. **Press detected** → wait 150 ms power-rail stabilization. If released within the wait, abort silently (mic never opens).
2. **Mic opens**: `ORB_USER_TALK` (green), start streaming 30 ms PCM frames as `{"user_audio_chunk":"<base64>"}`.
3. **Release**:
   - **`press_duration < 1000 ms`** → silent revert. No pad. No turn end. Back to `ORB_MUTED`.
   - **`real_audio_ms < 800 ms`** (press ≥ 1000 ms but only N frames actually streamed — RX hiccup) → short-turn skip. Same outcome.
   - **Otherwise**: `ORB_LOADING` (yellow), send 27 × 30 ms silence frames so server VAD trips end-of-utterance, then idle.

After the agent responds (playback_task drains audio and sees 250 ms of silence) the
orb returns to `ORB_MUTED`, ready for the next PTT.

### Keepalive

Just `ping → pong` per ElevenLabs docs and Python SDK source. No periodic
`user_activity` (that's an app-triggered "user is here" hint, not a timer).
`inactivity_timeout=120` URL parameter extends server-side cutoff from the 20 s default.

---

## LVGL 9 migration notes

Migrated from LVGL 8.2 → 9.x via `esp_lvgl_port` (Espressif's wrapper). Notes for
future maintenance:

- **Use `esp_lvgl_port`**, not raw `lv_display_create` — it handles the RGB-panel tearing avoidance + DMA framebuffer pickup correctly.
- **RGB panel needs `num_fbs = 2`** (`CONFIG_EXAMPLE_DOUBLE_FB=y`) when `avoid_tearing = true`.
- **`direct_mode = true`** in `lvgl_port_display_cfg_t.flags` — without it the port falls through to PARTIAL render mode with two full FBs, which leaves stale strips.
- **`CONFIG_LCD_RGB_RESTART_IN_VSYNC=y`** prevents permanent horizontal-shift drift on reset (documented ESP-IDF gotcha).
- v9 renames: `lv_scr_act` → `lv_screen_active`, `lv_obj_clear_flag` → `lv_obj_remove_flag`. `LV_FONT_MONTSERRAT_*`, `LV_PART_MAIN`, `lv_color_hex` are unchanged.
- **`lv_obj_remove_style_all(scr)`** is required on the active screen — v9 default theme adds padding that throws off `LV_ALIGN_*`.

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
| Phase 1 — Orb UI / LVGL 9 / state machine | ✅ done |
| Phase 2 — WiFi + SNTP + Supabase config | ✅ done |
| Phase 2b — ElevenLabs WSS + persistent session | ✅ done (init JSON shape pending) |
| Phase 3 — NFC PN532 | ✅ done (tag table pending) |
| Phase 4 — I2S audio (TX + RX) | ✅ done |
| Phase 5 — PTT | ✅ done |
| Phase 6 — NFC tag table (AGENT_START / TEST / custom phrase) | not started |
| Phase 7 — Battery monitoring per BEHAVIOR.md §9 | partial — GPIO4 ADC + 2 s moving average + load-anchored % gauge wired; idle-gating N/A (no idle state); current-sense N/A (no INA219). See [BATTERY.md](BATTERY.md). Remaining: real brown-out `EMPTY` anchor, harness-resistance fix |
| Phase 8 — Telemetry POST | not started |
| Phase 9 — Interface polish (this is what we're moving to next) | next |

---

## What's proven end-to-end

- Boot → WiFi → SNTP → Supabase config → SPLASH
- Tag scan → WSS open → agent greeting (Zane) plays through speaker
- Hold button → mic streams 30 ms PCM chunks to server
- Release → silence pad → agent transcribes + responds → response plays
- Loop for many turns within the same WSS
- Re-scan tag → fresh config fetch (volume / agent change applies live) → restart conversation
- LCD state transitions match what's happening (BOOT / WIFI / CONFIG / SPLASH / LOADING / USER_TALK / AGENT / MUTED)
- UDP logs survive bursts; on-screen state survives WiFi blips
