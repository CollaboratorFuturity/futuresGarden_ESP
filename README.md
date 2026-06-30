# The Orb — ESP32-S3 voice agent

Firmware for a Waveshare **ESP32-S3-Touch-LCD-2.8C** board running a single-purpose
voice agent UI ("The Orb"): WiFi → fetches per-device config from Supabase →
opens a persistent ElevenLabs Conversational AI WebSocket → push-to-talk via the
GPIO 0 button → speaker/mic over I2S on the J9 header → NFC tap to start or
restart a conversation.

End-to-end conversation works today. See [PROGRESS.md](PROGRESS.md) for the full
phase-by-phase status and what's still TODO.

---

## ⚠️ Serial monitor is dead once audio comes up

This is the single most important thing to remember. **Read it before debugging.**

The J9 audio header uses GPIO 19/20/43/44. GPIO 19/20 are the only native
USB-CDC pins on ESP32-S3, and GPIO 43/44 are the UART0 console pins (wrapped
by the on-board CH340 bridge as the "COM" USB-C). The moment `i2s_audio_init()`
configures those pins for I2S, **both USB-C ports stop enumerating** and
`esp_log` output goes nowhere over USB. There is no software toggle.

What this means:

- **No `printf` / `ESP_LOGI` via USB** once audio is up. Plan diagnostics around:
  on-screen state changes, audible cues from the speaker, and the **UDP log sink**
  (see Diagnostics below).
- **Auto-reset over DTR/RTS only works while USB enumerates.** Once audio
  init has run, the chip can't be reset over USB — fall back to the
  BOOT-button-while-tapping-RESET dance.
- Audio currently comes up **in `app_main`** (before WiFi) so USB dies within
  the first second of boot. The window for `idf.py flash` is small — start
  it before pressing RESET.

---

## Hardware

Board: **Waveshare ESP32-S3-Touch-LCD-2.8C**.
- 480×480 round display (ST7701S RGB) — output only, **touch removed**.
- TCA9554PWR I/O expander on the main I2C bus.
- SD card slot (SPI), battery ADC, octal PSRAM 8 MB.

External add-ons:

| What | Header | Address / Pins | Notes |
|---|---|---|---|
| Elechouse PN532 NFC | J8 (4-pin I2C) | 0x24 7-bit | DIP switches **SW1=ON, SW2=OFF** for I2C mode. Connect to header marked "I2C", NOT "UART". |
| MAX98357A speaker amp | J9 | See [`j9-audio-wiring.md`](j9-audio-wiring.md) | VIN to 3V3, GAIN tied (not floating), solder the data lines. |
| INMP441 mic | J9 (GPIO 20 / SD) | Shares BCLK + WS with MAX | 24-bit data in 32-bit slot, L/R pin to GND for left channel. **GPIO 20, not GPIO 0** — see note in wiring doc. |
| PTT button | GPIO 0 → GND | Active low | Internal/external strap pull-up holds line HIGH when released. **Don't hold at boot** — pulls chip into ROM download mode. Ground via the RTC battery header for star-grounding (keeps button bounce out of the mic GND wire). |

The board has two USB-C ports: one through a CH340 bridge ("COM"), one native
USB ("USB"). **Both are unusable while J9 is occupied.**

---

## Build / flash workflow

```
idf.py reconfigure   # only when CMakeLists.txt, sdkconfig.defaults, or idf_component.yml changed
idf.py build         # incremental build
idf.py flash         # incremental rebuild + flash
idf.py monitor       # only useful BEFORE audio init runs at boot
```

**Before first flash, you must:**

1. Fill in real WiFi credentials in [`main/secrets.h`](main/secrets.h) (gitignored, scaffold from `secrets.h.example`).
2. Set `EL_API_KEY` in `secrets.h` to your ElevenLabs API key.
3. Set the PN532 DIP switches to I2C mode.

`certs/gts_root_r4.pem` (Supabase) and `certs/gts_root_r1.pem` (ElevenLabs) are
both already populated. Don't rotate them without re-flashing.

**Iteration with audio enabled:** because USB dies almost immediately, your
flashing window is short. Start `idf.py flash` BEFORE pressing RESET, and the
host-side tooling waits for the bootloader to appear.

---

## Boot flow

```
app_main:
  Driver_Init           # I2C + EXIO + battery loop
  LCD_Init / SD_Init / LVGL_Init / orb_ui_init   # display up, ORB_BOOT
  NFC_Init              # spawns PN532 polling task (core 0)
  button_init           # GPIO 0 ISR + 6 KB debounce task
  i2s_audio_init        # *** USB DIES HERE ***
  Wireless_Init         # spawns WiFi task → event handler drives BOOT→WIFI→CONFIG→LOADING

WIFI_EVENT_STA_GOT_IP fires post_connect_task (8 KB stack):
  log_sink_start(6666)  # UDP sink FIRST — USB-CDC already dead, so a crash in any
                        #   later step is still visible over UDP
  orb_sntp_sync(15s)    # clock must be right before any TLS cert validation
  orb_ota_check_and_update_on_boot()   # OTA recovery floor — BEFORE config + convai
                        #   (on a successful update this restarts and never returns)
  orb_ui_set_state(ORB_CONFIG)
  orb_refresh_config()  # GET https://…/get-device-config → agent_id, agent_name, volume
                        #   (applies i2s_audio_set_volume_pct(cfg.volume * 10) internally)
  orb_ui_set_state(ORB_LOADING)
  convai_start(agent_id)  # WSS opens → greeting plays. No idle SPLASH — the device
                        #   boots straight into a persistent conversation.

On NFC tag scan (only fires in ORB_MUTED — polling is gated to the idle window):
  uid → colon-hex ("04:38:17:9A:CB:2A:81")                   # matches nfc_tags.json keys
  if !s_tags: nfc_tags_reload()                              # lazy download if boot warm-up missed
  phrase = s_tags[uid]                                       # downloaded from GitHub (NFC_TAGS_URL)
  if phrase is None: log "unmapped — ignoring"; return       # no tone, no state change

  orb_ui_set_state(ORB_NFC); blopp tone                      # ack only for a mapped card

  if phrase not in {AGENT_START, TEST} and convai_is_running():
    orb_ui_set_state(ORB_LOADING)
    convai_send_user_message(phrase)                         # inject into LIVE session, no restart
    return
  # AGENT_START / TEST (or phrase with no live session): reload + restart
  nfc_tags_reload()
  if convai_is_running(): convai_stop(); wait 200 ms        # ORB_NFC stays up
  orb_refresh_config()                                       # volume / agent changes apply live
  orb_ui_set_state(ORB_LOADING)                              # amber + sweep: WSS connecting
  convai_start(agent_id) → WSS opens → greeting plays        # Convai tasks drive AGENT / USER_TALK / MUTED
```

---

## State machine — what you see on screen

`OrbState` in [`LVGL_UI/orb_ui.h`](main/LVGL_UI/orb_ui.h):

The UI is a single screen that re-skins itself per state: a central circle
(colour + animation vary by state), an agent-name label and a state label
inside it, a sweeping arc around it during "working" states, and a battery
readout at the top. State changes cross-fade through opacity 0 (200 ms out →
swap → 200 ms in).

| State | Colour | Label | Motion | When |
|---|---|---|---|---|
| `ORB_BOOT` | grey `#404040` | "Booting" | — | Power-on through `Wireless_Init` |
| `ORB_WIFI` | grey `#404040` | "Connecting WiFi" | sweep arc | WiFi associating |
| `ORB_CONFIG` | grey `#404040` | "Fetching config" | sweep arc | Got IP, fetching Supabase config |
| `ORB_CHECK_UPD` | grey `#404040` | "Checking for updates" | sweep arc | OTA update check |
| `ORB_UPDATING` | grey `#404040` | "Updating" | sweep arc | OTA update in progress |
| `ORB_WIFI_FAIL` | red `#C0392B` | "No WiFi. Check router & restart" | — | WiFi association failed |
| `ORB_SPLASH` | dim grey `#2A2A2A` | "Ready" | — | Idle, no WSS open |
| `ORB_LOADING` | amber `#C47A0C` | "Loading" | sweep arc | WSS connecting, or waiting for agent response after a turn |
| `ORB_USER_TALK` | green `#27AE60` | "Listening" | pulse (200→300) | PTT held, mic streaming |
| `ORB_AGENT` | blue `#1A7DC4` | "Agent speaking" | pulse (200→220) | Agent audio playing |
| `ORB_MUTED` | dim grey `#2A2A2A` | "Press to talk" | breath (200→212) | Inside an open WSS, between turns |
| `ORB_LOW_BAT` | red `#C0392B` | "Low battery" | — | Reserved for §9 battery work (not yet wired) |
| `ORB_ERROR` | red `#C0392B` | "ERROR Restart the Orb." | red breath | Fatal error surfaced to user |
| `ORB_NFC` | purple `#7B2FA8` | "NFC Scanned" | — | NFC tag scanned; set before the blopp tone, held through the config fetch, then replaced by `ORB_LOADING` |

States, colours, labels and motion presets are authoritative in
[`LVGL_UI/orb_ui.c`](main/LVGL_UI/orb_ui.c) (`STATE_STYLE[]`); design tokens
live in `UI_SPEC.md`.

Cross-task UI updates use a "set atomic flag, render on next LVGL tick"
pattern — no mutex. See `orb_ui.c`.

---

## Diagnostics that DO work once audio is up

Because USB is dead, plan around these:

### UDP log sink (primary)

`log_sink_start(6666)` runs the moment WiFi gets IP. From then on, every
`ESP_LOGI/W/E` is broadcast over UDP to your laptop. On the host:

```bash
nc -ul 6666
```

If your AP throttles broadcast, set `LOG_SINK_TARGET_IP` to the laptop's IP
in [`main/Wireless/log_sink.c`](main/Wireless/log_sink.c). Currently hard-coded
to `192.168.1.27` — update for your environment.

Tip: use `Ctrl-C` to stop `nc`, not Ctrl-Z (suspending leaves the port bound
and the next `nc` start fails with "Address already in use").

#### Heartbeat line — the primary health surface

Every 2 s while `convai` is running (when `s_log_verbose` is on — currently
on by default), `heartbeat_task` emits one wide diagnostic line. Read it
left-to-right as a session timeline:

```
hb: ws=up rx_age=4ms tx_age=989ms rx_2s=73662B last=agent_chat_response_part
    lvgl_d=6134 ptt=0 heap_int=11K psram=3196K conn#1 disc#0
    pings=6 pongs=3 dropped=0 skip_ptt=3 pp_delta=0
    q_depth=0B q_block=0 q_retry=0
```

| Field | Meaning | Healthy value |
|---|---|---|
| `ws=up/DOWN` | Lib's view of connection state | `up` |
| `rx_age` | ms since last byte received from server | < 2000 ms with normal ping cadence |
| `tx_age` | ms since last successful send | < 1500 ms |
| `rx_2s` | bytes received in the last 2 s window | varies; high during agent audio |
| `last=` | type of last server message | `ping` between turns, `audio`/`agent_response` during reply |
| `lvgl_d` | LVGL task iterations in last 2 s — proves screen isn't frozen | > 1000 |
| `ptt=0/1` | PTT button currently held? | matches button state |
| `heap_int=NK` | free internal SRAM | > 4 KB; tightens during PTT |
| `psram=NK` | free PSRAM | plenty (we have 4+ MB free) |
| `conn#N disc#N` | cumulative WS connect / disconnect counts | `conn#1 disc#0` for a healthy session |
| `pings=N pongs=N` | cumulative server pings received / pongs we sent | should track each other |
| `dropped=N` | pong sends that returned ≤ 0 (failed) | 0 — non-zero means server will tear us down |
| `skip_ptt=N` | pongs intentionally skipped during PTT (by design) | rises during PTT, fine |
| `pp_delta` | `pings - pongs - skip_ptt` — *unintentional* gap | 0 (or transient 1) |
| `q_depth=NB` | bytes currently in the send queue | 0 in steady state, may rise during PTT, drains within seconds |
| `q_block=N` | cumulative times producer blocked because queue was full | 0 unless link is genuinely down |
| `q_retry=N` | cumulative `send_text` failures (sender retries) | 0 on a healthy link; rises = transient congestion |

Plus `sender: sent #N (item=NB, last_fail_streak=K)` every 33 successful
mic sends — confirms `sender_task` is alive and how often it had to retry.

### Visual cues

- **On-screen state** — the orb state changes are the simplest live feedback (LOADING / USER_TALK / AGENT / MUTED).
- **Beeps** — `i2s_audio_beep(freq, duration_ms)` from any task; pick distinct freqs to flag distinct code paths.
- **Tone sequences** — `i2s_audio_tone_sequence(...)` for compound cues (the NFC "blopp" is a 3-tone sequence).

### What does NOT work

- `printf` / `ESP_LOGx` to USB after `i2s_audio_init`.
- Re-flashing via DTR auto-reset after the audio pins are claimed.

---

## File layout

```
main/
├── main.c                          # app_main + boot order + button callback
├── secrets.h                       # WiFi/Supabase/EL keys (gitignored)
├── secrets.h.example               # template (committable)
├── EXIO/TCA9554PWR.{c,h}           # I/O expander
├── I2C_Driver/                     # Shared I2C bus (TCA9554 + PN532)
├── LCD_Driver/ST7701S.{c,h}        # RGB panel + backlight
├── LVGL_Driver/                    # LVGL 8.4 display registration (direct, no esp_lvgl_port) — see SCREEN.md
├── LVGL_UI/orb_ui.{c,h}            # Orb state machine + widgets
├── Wireless/
│   ├── Wireless.{c,h}              # WiFi connect + event handler + agent_id cache
│   ├── config_fetch.{c,h}          # SNTP + HTTPS GET + cJSON parse (Supabase config; GitHub NFC tag-table download)
│   └── log_sink.{c,h}              # UDP log redirect (esp_log_set_vprintf)
├── NFC/nfc.{c,h}                   # PN532 driver, 100 ms polling, 1.5 s debounce, GitHub-downloaded tag table + dispatch (gated to ORB_MUTED)
├── Audio/i2s_audio.{c,h}           # Split I2S (TX=I2S0 master, RX=I2S1 slave) + DOUT mute
├── Button/button.{c,h}             # GPIO 0 PTT button (50 ms debounce, edge-trigger)
├── Convai/convai.{c,h}             # ElevenLabs ConvAI WSS client + PTT mic-stream task + playback task + send queue (1 MB PSRAM) + sender_task
├── SD_Card/                        # Stock Waveshare driver
├── BAT_Driver/                     # GPIO4 ADC battery sense + averaging — see BATTERY.md
├── certs/
│   ├── gts_root_r4.pem             # Supabase root cert
│   └── gts_root_r1.pem             # ElevenLabs root cert
└── CMakeLists.txt                  # IDF 5.5 PRIV_REQUIRES list
```

---

## ElevenLabs ConvAI protocol — what we send / receive

See [PROGRESS.md](PROGRESS.md) for the full Convai module description. Quick summary:

**Client → server** (text JSON frames):
- `{"type":"conversation_initiation_client_data", ...}` once at connect
- `{"type":"pong","event_id":N}` on every server ping (skipped during PTT — see CLAUDE.md)
- `{"type":"user_activity"}` every 60 s while idle (proactive keepalive, matches legacy Pi)
- `{"user_audio_chunk":"<base64>"}` while PTT is held + 43 silence frames after release (only if real turn)

**All outbound mic frames go through the app-level send queue** (1 MB PSRAM xRingbuffer + dedicated `sender_task`) — direct `esp_websocket_client_send_text` calls from the mic loop are forbidden. The queue absorbs transient upstream congestion so the WS lib never sees a fail-fast pattern and never triggers a spurious disconnect. Full architecture, lifecycle, failure modes and diagnostics workflow live in [CONVAI.md](CONVAI.md).

**Server → client** (text JSON frames, handled in `Convai/convai.c::process_message`):
- `conversation_initiation_metadata` — confirms session
- `audio` (single events up to several hundred KB, reassembled from WS fragments, base64-decoded in chunks straight to PCM ring)
- `agent_response` / `user_transcript` — logged for visibility
- `interruption` — flushes PCM ring
- `ping` — we reply with `pong`

**PTT timing** (in `Convai/convai.c::mic_task`, all from BEHAVIOR.md §3.1 unless noted):

| Constant | Value |
|---|---|
| Debounce | 50 ms |
| Power-rail wait after press accepted | 150 ms |
| Silent-revert threshold | press < 1000 ms |
| Frame | 30 ms / 480 samples / 960 bytes |
| Tail silence pad | 43 frames ≈ 1290 ms (spec is 1500 ms — shortened by project decision; must exceed the server VAD threshold pinned to 800 ms) |
| Short-turn skip (real audio) | < 800 ms |

---

## ESP-IDF 5.5 component dependencies (gotcha)

We use explicit `PRIV_REQUIRES` in [`main/CMakeLists.txt`](main/CMakeLists.txt).
Adding any new ESP-IDF include means adding its component to the list, or
you'll get `fatal error: …/foo.h: No such file or directory`. IDF split the
old monolithic `driver` into `esp_driver_*`. Reference table:

| Header | Component |
|---|---|
| `driver/{gpio,i2c_master,ledc,sdmmc_host,spi_master}.h` | `esp_driver_{gpio,i2c,ledc,sdmmc,spi}` |
| `driver/i2s_std.h` | `esp_driver_i2s` |
| `esp_lcd_*.h` | `esp_lcd` |
| `esp_adc/*.h` | `esp_adc` |
| `esp_flash.h` | `spi_flash` |
| `esp_mac.h` | `esp_hw_support` |
| `esp_sntp.h` | `lwip` |
| `esp_netif*.h`, `esp_event.h` | `esp_netif`, `esp_event` |
| `esp_vfs_fat.h`, `sdmmc_cmd.h` | `fatfs`, `sdmmc`, `vfs` |
| `esp_wifi.h`, `nvs_flash.h`, `esp_bt*.h` | `esp_wifi`, `nvs_flash`, `bt` |
| `esp_http_client.h` | `esp_http_client` |
| `esp_websocket_client.h` | `esp_websocket_client` (managed) |
| `esp_crt_bundle.h` / `esp_tls.h` | `esp-tls` |
| `mbedtls/*.h` | `mbedtls` |
| `cJSON.h` | `cjson` (managed) |
| `lvgl.h` | `lvgl` (managed, pinned `~8.4`; do NOT use `esp_lvgl_port`) |

Managed components are pulled via [`main/idf_component.yml`](main/idf_component.yml).

---

## Known gotchas (the short list)

1. **RGB panel tearing** — only goes away with Travis's exact bounce-buffer + vsync-sem + single-FB combo on LVGL 8.4 / IDF 5.5. See [SCREEN.md](SCREEN.md) for the full config and the list of approaches that DON'T work.
2. **GPIO 0 strap pull-up masks INMP441 SD line** — mic is on GPIO 20, NOT GPIO 0. Don't move it back.
3. **Don't hold the PTT button at boot** — GPIO 0 LOW at reset = ROM download mode.
4. **WSS task stack ≥ 8 KB** — cJSON + mbedTLS + base64 decode overflow the 4 KB default.
5. **NFC task stack ≥ 8 KB** — `handle_uid` can do two sequential HTTPS GETs with TLS (tag-table download + `orb_refresh_config()`); the 4 KB default overflows. They run one at a time, so 8 KB holds.
6. **Pin TLS roots per host** — the built-in CA bundle didn't match either Supabase or ElevenLabs reliably on our build. We embed `gts_root_r4.pem` (Supabase) and `gts_root_r1.pem` (ElevenLabs).
7. **WSS audio events are huge** — single events for short greetings have been seen at 250+ KB. The reassembly buffer is 2 MB PSRAM. Decode in 1 KB chunks with `portMAX_DELAY` backpressure so the PCM ring (128 KB) doesn't have to hold a whole turn.
8. **DOUT mute (not channel-disable)** to silence the speaker during recording — TX channel must stay enabled or the mic loses BCLK/WS.
9. **Battery ADC reads *board* voltage, not cell** — it senses after the power switch + harness, so it's load-dependently ~20–190 mV below the cell. Gauge constants are anchored to that post-drop reading, not cell voltage. See [BATTERY.md](BATTERY.md). Don't re-add idle-gated sampling — there's no idle state.
