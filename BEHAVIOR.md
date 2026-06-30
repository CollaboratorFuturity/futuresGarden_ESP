# Orb Behavioral Contract

This document defines what the Orb firmware must **do** at runtime. It is platform-neutral — the Raspberry Pi build (`main.py` et al.) implements it today; the ESP32-S3 firmware will implement it tomorrow. Numbers cited here are extracted from the Pi source and are the canonical values until superseded.

Companion docs: [HARDWARE.md](HARDWARE.md) (GPIO/peripherals), [README.md](README.md) + [CONVAI.md](CONVAI.md) + [SCREEN.md](SCREEN.md) (ESP-IDF implementation design).

> **Path note:** all `*.py` source citations below (`main.py`, `nfc_backend.py`, `mute_button.py`, `battery_log.py`, `constants.py`, `serial_com.py`, `config_fetcher.py`) refer to the legacy Raspberry Pi production firmware, which is **not** in this repo — they're cited for behavioral reference only. Line numbers are relative to that Pi source.

---

## 1. State machine

Two states.

| State | Meaning | Display | NFC | Mic | WSS |
|---|---|---|---|---|---|
| `splash_idle` | Default. Waiting for an `AGENT_START` tag. | `S` (splash) | enabled | closed | closed |
| `running_agent` | Conversation in progress with ElevenLabs. | `L`/`U`/`M`/`O` per turn | enabled in idle/agent turns; disabled during user turn audio capture | gated by button | open and persistent across turns |

### Transitions

| From | Event | To | Action |
|---|---|---|---|
| `splash_idle` | `AGENT_START` tag | `running_agent` | Open WSS, send init JSON, play greeting (first turn) |
| `running_agent` | `AGENT_START` tag | `splash_idle` | Treat as TEST: close WSS, hot reload, play greeting via temp WSS |
| any | `TEST` tag | `splash_idle` | Hot reload (re-fetch config), play greeting via temp WSS |
| `running_agent` | shutdown signal | (exit) | Close WSS cleanly, write 'B' to display |

Custom-phrase NFC tags (any UID mapped to a non-reserved string) are **not** state transitions — see §6.

Source: `main.py:96` (`STATE = "splash_idle"`), `main.py:303-339` (NFC handler), `main.py:476-628` (hot reload).

---

## 2. Conversation lifecycle (inside one WSS session)

### 2.1 Connect

Open `wss://api.elevenlabs.io/v1/convai/conversation?agent_id={AGENT_ID}&inactivity_timeout=60` with header `xi-api-key: {API_KEY}`. Send `conversation_initiation_client_data`:

```json
{
  "type": "conversation_initiation_client_data",
  "conversation_config_override": {
    "agent": { "language": "en" },
    "tts": { "output_audio_format": "pcm_16000", "volume": <optional> },
    "asr": { "input_audio_format": "pcm_16000" }
  }
}
```

`tts.volume` is included **only when the active agent has `tts_volume` set in config** (the "Nova quirk" — currently the Pi hardcodes this against an agent_id at `main.py:361-362, 1182-1183`; on ESP32 it is config-driven).

### 2.2 First-turn greeting

Track a session-local `did_init` flag (initially `false`).

- If `did_init == false`: do **not** send `SUPPRESS_GREETING`. Immediately call the agent-response receiver with `first_turn=true, barge_after_ms=500`. The agent will speak its opening message; if greeting audio arrives within 500ms of session start, the receiver may open the mic mid-greeting (first-turn barge-in). Set `did_init = true` once the greeting flow ends — including on greeting failure (so the user is not stuck in silent retry loops).

- If `did_init == true` (reconnect within session): send `SUPPRESS_GREETING` instead:

```json
{
  "type": "conversation_initiation_client_data",
  "conversation_config_override": { "agent": { "first_message": "" } }
}
```

Source: `main.py:1178-1237` (`did_init`, SUPPRESS_GREETING), `main.py:1087-1093` (first-turn barge), `main.py:1228-1235`.

### 2.3 Per-turn loop

```
loop:
  user_turn()                   # see §3
  if real_audio_ms < 800:        # short-turn skip, §2.4
    continue
  agent_turn()                  # see §4
  resume keepalive (§5)
```

### 2.4 Short-turn skip

After the user turn ends, examine the metrics from the audio capture: `ms_sent` minus `synthetic_ms_sent` (the silence pad). If real audio sent is **< 800ms**, skip the agent-response wait and loop immediately. Prevents the agent from speaking in response to empty/accidental presses.

Source: `main.py:1273-1289`.

---

## 3. PTT semantics (the only input mode)

**The button gates the microphone. There is no client-side VAD.** ElevenLabs handles silence detection server-side once we stop sending audio.

### 3.1 Constants

| Symbol | Value | Source |
|---|---|---|
| `DEBOUNCE_MS` | 50 ms | `mute_button.py:66` |
| `PRESS_MIN_MS` (silent revert threshold) | 1000 ms | `mute_button.py:65` |
| Power-rail stabilization on press | 150 ms | `main.py` PTT path |
| Frame duration | 30 ms | `main.py:60` |
| `SAMPLES_PER_FRAME` | 480 (16000 Hz × 30 ms / 1000) | `main.py:62` |
| `BYTES_PER_SAMPLE` | 2 | `main.py:63` |
| `FRAME_BYTES` | 960 | `main.py:64` |
| `END_SILENCE_CHUNKS` (silence pad on release) | 50 frames = 1500 ms | `main.py:76` |

### 3.2 Idle (no turn active)

- Display = `M` (muted).
- Button: HW pull-up, GPIO active-low, polled at ~10 ms.
- WSS keepalive task running (§5).

### 3.3 Press → start

1. Detect edge. Wait `DEBOUNCE_MS`. If still pressed, accept the edge.
2. Wait 150 ms for power-rail stabilization.
3. Cancel keepalive task immediately (`maintain_pong()` must not consume audio messages while the user speaks).
4. Display = `U` (unmuted).
5. Begin streaming 30 ms PCM frames as `{"user_audio_chunk": "<base64>"}`.

Source: `main.py:682-820, 757-761`.

### 3.4 Release → end

Compute `duration_ms` since press.

- If `duration_ms < PRESS_MIN_MS` (i.e. < 1000 ms): **silent revert.** Re-mute, display back to `M`, do **not** send a silence pad, do **not** end the turn server-side. Loop continues.
- If `duration_ms ≥ PRESS_MIN_MS`: append `END_SILENCE_CHUNKS` (50 frames = 1500 ms) of zero audio. Display = `L` (loading). Record `LAST_MIC_METRICS` (`ms_sent`, `synthetic_ms_sent`). Return; the agent-turn handler runs next.

Source: `mute_button.py:111-130`, `main.py:1273` (metrics).

### 3.5 NFC interrupt during press

If the global `force_turn_end` event is set (from a custom-phrase NFC tag, see §6), append the silence pad and return immediately, regardless of press duration. The turn ends and the injected phrase reaches the agent next.

Source: `nfc_backend.py:284-285`, `main.py` PTT path.

---

## 4. Agent response handling

### 4.1 Constants

| Symbol | Value | Source |
|---|---|---|
| `FIRST_CONTENT_MAX` | 15.0 s (default), 1.0 s if last user turn was < 800 ms real audio | `main.py:79, 1065-1067` |
| `CONTENT_IDLE` | 0.15 s | `main.py:80` |
| `GRACE_DRAIN` | 0.15 s | `main.py:81` |

### 4.2 Receive loop

1. Display = `L`.
2. Open speaker output buffer (`out_buf`).
3. Wait up to `FIRST_CONTENT_MAX` (or 1.0 s adaptive cap) for the first inbound `audio` event.
4. On first audio: display = `O` (agent speaking). Decode base64 in chunks, append to `out_buf`, drain full 960-byte frames to I2S/ALSA as they accumulate.
5. Continue receiving until `now - last_content_at > CONTENT_IDLE`.
6. **Grace drain:** wait up to `GRACE_DRAIN` for stragglers. If any audio/text arrives in this window, return to step 5. Otherwise end the turn.
7. Final frame: zero-pad to 960 bytes, write, close speaker buffer.
8. Display ← idle scene (`M` between turns).
9. Restart keepalive task (§5).

### 4.3 Streaming base64 decode

Do **not** accumulate the full `audio_base_64` field. Decode in chunks straight into the playback ring buffer. Filter out non-`audio` event fields server-side as much as possible to keep JSON parse work small. (On Pi this happens because Python's `json.loads` is one-shot per message; on ESP32 use `ArduinoJson::JsonDocument::filter` to skip uninteresting fields and decode the audio string with `mbedtls_base64_decode` in 4-char chunks.)

Source: `main.py:966-1168` (`receive_response`).

---

## 5. Keepalive

Two mechanisms: outbound activity ticks and inbound ping reply.

### 5.1 Outbound `user_activity`

Every 60 s of idle, send `{"type":"user_activity"}`. Prevents ElevenLabs' inactivity timeout from closing the session (note `&inactivity_timeout=60` on the WSS URL).

Source: `main.py:208-249, 211, 219`.

### 5.2 Ping → pong

On `{"type":"ping","event_id":<n>,"ping_ms":<m>}` from server, reply with `{"type":"pong","event_id":<n>}`. Match `event_id` exactly. The optional `ping_ms` value is the server's hint for how long the device should wait before replying; clients may honor it but a prompt reply is always acceptable.

### 5.3 Cancellation discipline

The keepalive task **must be cancelled the instant the user starts speaking** (button press in PTT). Otherwise it can race with the audio path and consume `user_audio_chunk` messages that the receive loop expects. Restart the keepalive only after the agent response completes (after the grace drain in §4).

Source: `main.py:757-761, 921` (cancel on speech start), `main.py:1290+` (restart after agent turn).

---

## 6. NFC tag semantics

### 6.1 Library format

JSON map of UID → phrase. UIDs are normalized to `XX:XX:XX[:XX]` uppercase hex. Phrases are arbitrary UTF-8 strings. Two reserved keys are special-cased:

```json
{
  "04:A1:B2:C3":  "What is the weather today?",
  "04:D4:E5:F6":  "Tell me a joke",
  "TEST":         "TEST",
  "AGENT_START":  "AGENT_START"
}
```

The library is fetched from a configured URL on boot/hot-reload and cached in NVS as offline fallback.

> **ESP32 note:** the library is **downloaded from GitHub each session** (`NFC_TAGS_URL` in `secrets.h` → a raw file on the public OTA repo, fetched via the IDF cert bundle in `config_fetch.c::orb_nfc_tags_fetch()`). There is **no NVS cache / no offline fallback** — the orb needs internet to converse at all, so an offline-only table would never be exercised. UID keys are colon-hex uppercase (`uid_to_hex` matches the JSON format exactly). The table is reloaded on every AGENT_START/TEST scan, so editing the GitHub file lands on the next scan with no reflash (subject to raw.githubusercontent.com's ~5 min CDN cache).

### 6.2 Tag table

| Tag | State at scan | Behavior |
|---|---|---|
| `AGENT_START` | `splash_idle` | Transition to `running_agent` (open WSS, init, greeting). |
| `AGENT_START` | `running_agent` | Treat as `TEST` — close WSS, hot reload, play greeting via temp WSS, return to `splash_idle`. |
| `TEST` | any | Hot reload config, play greeting via temp WSS, return to `splash_idle`. No conversation initiated. |
| Any other mapped UID | `running_agent` | Inject `{"type":"user_message","text":"<phrase>"}` over WSS, set `force_turn_end` to end the current user turn. The agent receives the phrase as user input and responds. |
| Any other mapped UID | `splash_idle` | No-op (queue the phrase for next session, or simply ignore — current Pi behavior queues with `maxlen=16`). |
| Unmapped UID | any | Ignore. |

Source: `nfc_backend.py:267-298`, `main.py:303-339`.

> **ESP32 note:** this firmware has **no `splash_idle` state** — it boots straight into a persistent `running_agent` session. So the table collapses: `AGENT_START` and `TEST` both mean "reload config + tag table, then restart the session" (there's no separate splash/temp-WSS greeting). A custom-phrase tag injects `{"type":"user_message","text":"<phrase>"}` into the live WSS via `convai_send_user_message()` with no restart. **`force_turn_end` IS required** (verified on hardware): the session uses server-side VAD turn detection on the *audio* stream (`turn_detection.silence_duration_ms`, §2.1), so a text-only `user_message` gives the server no end-of-turn and the agent never replies (symptom: stuck on the loading scene). So after sending the text, `convai_send_user_message()` appends the same tail-silence pad a PTT release sends (§3.4) to close the turn — *this is the ESP32 realization of `force_turn_end`*, and unlike the Pi it fires unconditionally (not only mid-press), because NFC scans happen while PTT is idle and there's otherwise no audio turn boundary at all. A custom-phrase tag scanned with no live session (rare) falls back to a restart and the phrase is dropped. Unmapped UIDs are ignored exactly as specified (silently — no tone, no state change). Implemented in `nfc.c::handle_uid` + `convai.c::convai_send_user_message`.

### 6.3 Debounce

Same UID read within **1.5 s** of the previous read is ignored. Different UIDs bypass the debounce.

Source: `nfc_backend.py:39, 262`.

### 6.4 Enable/disable

NFC scanning is **disabled** during the user turn audio capture (prevents tag reads from corrupting the audio path) and **enabled** during agent response and idle. Implemented as an internal flag the polling loop checks.

Source: `nfc_backend.py:287` (disable on phrase inject), `main.py:1051, 1078, 1286` (enable around turns).

> **ESP32 note (stricter than the Pi):** scanning is allowed **only in the idle `ORB_MUTED` window** between turns — it is suppressed during user-turn capture (`ORB_USER_TALK`), while waiting for the agent (`ORB_LOADING`), **and while the agent is speaking (`ORB_AGENT`)**. The Pi keeps NFC live during the agent response; this build does not, so a tag can't interrupt or be read over a turn at all. Implemented at the single state-transition choke point: `orb_ui.c::orb_ui_set_state()` calls `NFC_Set_Polling(s == ORB_MUTED)`, flipping the same `s_polling_enabled` flag the poll loop checks.

### 6.5 Init failure (defensive)

If the PN532 cannot be initialized after 5 attempts, the polling task should **not** silently die. The Pi today has a known bug where the device sits in `splash_idle` forever with no feedback (TODO #8 in the legacy docs). The ESP32 implementation must surface this to the display (`V` overlay or a dedicated error scene) and retry periodically.

Source: `nfc_backend.py:172-229` (current bug — do not replicate).

---

## 7. Hot reload

**Trigger:** TEST tag, or AGENT_START during `running_agent`.

**What can change live:** `agent_id`, system volume, NFC tag library, `tts_volume` override.

**What cannot change live (require reboot):** audio format / sample rate, PTT timing constants, frame size, WSS endpoint scheme.

### 7.1 Sequence

1. Display = `L`.
2. HTTP GET the config endpoint with retry (5 attempts, 10 s timeout each).
3. Apply system volume (on Pi: `amixer set Speaker <raw>` from the `VOLUME_MAP` in `constants.py`; on ESP32: I2S DAC software gain or MAX98357A path-equivalent).
4. Update active `agent_id`. Reconstruct WSS endpoint URL.
5. If a session is currently active (`running_agent`), close the WSS cleanly and force `STATE = splash_idle`.
6. Open a temporary WSS, send init JSON, play the greeting only (no full conversation loop). Close.
7. Persist new values to NVS as last-known-good.
8. Display = `S`.

Source: `main.py:476-628` (`hot_reload_config`).

---

## 8. Display scenes

The Pi sends single-byte commands over UART. The ESP32 has its own LCD; the same enum names are retained for traceability but each value maps to a built UI scene rendered locally.

| ID | Name | When |
|---|---|---|
| `S` | splash | `splash_idle` (idle, waiting for `AGENT_START`) |
| `L` | loading | Waiting for agent response, or hot reload in progress |
| `U` | unmuted | Recording user audio (button held / mic open) |
| `M` | muted | Between turns inside `running_agent`; idle in PTT mode |
| `O` | agent speaking | Agent audio playback in progress |
| `N` | NFC scanned | Transient overlay on tag detection |
| `V` | low battery | Voltage between `LOW_VOLTAGE` and `CRITICAL_VOLTAGE` (3 consecutive readings) |
| `D` | dying | Voltage < `CRITICAL_VOLTAGE` or Pi under-voltage flag (3 consecutive readings) |
| `B` | boot | Application startup signal / clean shutdown |

Source: `serial_com.py` (Pi command table), legacy `README.md` Display Commands section.

---

## 9. Battery thresholds

### 9.1 Sensing path

**On Pi (legacy):** INA219 over I2C (address 0x43) — gives voltage and current.

**On ESP32-S3 (current):** the 2.8C's onboard battery-sense divider on `GPIO4` ADC. Voltage only — no current measurement. The ME6217C33M5G LDO + onboard charge IC handle charging without firmware involvement. See [HARDWARE.md](HARDWARE.md) and [BATTERY.md](BATTERY.md).

### 9.2 Sampling discipline

Voltage sampling is **gated to the `splash_idle` state only.** Reasons:

- The WiFi radio induces ADC noise on a shared analog rail; sampling during `running_agent` (continuous WSS streaming) gives unreliable readings.
- The user is not actively using the device in `splash_idle`, so a fresh reading is operationally meaningful — it tells us how much runtime is left for the next conversation.
- Avoids fighting the audio path for I/O time slices.

A 30 s FreeRTOS timer drives the sample. The state machine **enables** the timer on entry to `splash_idle` and **disables** it on entry to `running_agent`. Telemetry upload (when WiFi is available) runs on a 90 s timer with the same gating.

### 9.3 Thresholds (platform-independent — LiPo voltage curve)

| Symbol | Value | Source |
|---|---|---|
| `LOW_VOLTAGE` | 3.8 V | `battery_log.py:25` |
| `CRITICAL_VOLTAGE` | 3.7 V | `battery_log.py:26` |
| `VOLTAGE_THRESHOLD` (percent span) | 0.35 V (0% = 3.7 V, 100% = 4.05 V) | `battery_log.py:27` |
| `CHECK_INTERVAL` | 30 s | `battery_log.py:24` |
| `SEND_INTERVAL` | 90 s | `battery_log.py:23` |
| `LOW_COUNT_THRESHOLD` | 3 consecutive readings | `battery_log.py:29` |
| `CRITICAL_COUNT_THRESHOLD` | 3 consecutive readings | `battery_log.py:30` |

### 9.4 Reactions

- 3 consecutive critical readings (≤ `CRITICAL_VOLTAGE`): display `D`, cut backlight (ST7701 driver in `main/LCD_Driver/ST7701S.c`), then power off cleanly.
- 3 consecutive low readings (between `CRITICAL_VOLTAGE` and `LOW_VOLTAGE`, not yet critical): display `V` overlay.

### 9.5 Telemetry payload

Periodic POST during idle when WiFi is up:

```json
{ "device_id": "orb-XXXXXX", "voltage_V": 3.94, "percent": 68, "temperature_C": 42.1, "ts": <unix> }
```

`temperature_C` from IDF's internal `temperature_sensor_get_celsius` (chip die temp). The Pi build also reported `current_mA` from the INA219; that field is **omitted** on the ESP32 path.

**Charging detection:** none in firmware. The 2.8C's onboard charge LED indicates state visually; the ADC voltage will trend upward when charging and the percent calculation reflects that. No explicit "charging" flag is exposed.

---

## 10. Audio constants

| Symbol | Value | Source |
|---|---|---|
| `RATE` | 16000 Hz | `main.py:57` |
| `CHANNELS` | 1 (mono) | `main.py:58` |
| `FORMAT` | 16-bit signed little-endian | `main.py:59` |
| `FRAME_MS` | 30 ms | `main.py:60` |
| `SAMPLES_PER_FRAME` | 480 | `main.py:62` |
| `BYTES_PER_SAMPLE` | 2 | `main.py:63` |
| `FRAME_BYTES` | 960 | `main.py:64` |

**Codec:** PCM 16 kHz is the default (audio path in [CONVAI.md](CONVAI.md)). μ-law 8 kHz was specced as a battery-life fallback; not wired in the current ESP-IDF build.

---

## 11. Volume calibration

The Pi's `VOLUME_MAP` (1–10 → ALSA raw) is hardware-specific to the Pi's amplifier path. The ESP32 has different output hardware (MAX98357A in BTL mode); a new calibration table must be derived empirically against the same speaker. Until then, volume 1–10 maps to a software gain on the I2S TX path.

Source: `constants.py:9-20`.

---

## 12. What is *not* part of this contract

- ALSA-specific double-open workarounds.
- Single-byte UART display protocol (the names live on as scene IDs, the wire protocol does not).
- Multi-process battery service / systemd unit boundaries.
- `config_fetcher.service` as a separate process.
- Filesystem-based logging.
- Client-side WebRTC VAD.
- PNG / asset-file display pipeline.

These are Pi platform artifacts, intentionally dropped in the ESP-IDF build.
