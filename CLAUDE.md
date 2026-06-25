# Orb — critical rules (easy to get wrong)

Full specs: [`BEHAVIOR.md`](BEHAVIOR.md) (platform-neutral runtime contract) · [`PROGRESS.md`](PROGRESS.md) (status) · [`HARDWARE.md`](HARDWARE.md) (GPIO/peripherals).
ESP-IDF firmware design lives in [`README.md`](README.md) (overview + diagnostics), [`CONVAI.md`](CONVAI.md) (WSS client), and [`SCREEN.md`](SCREEN.md) (LVGL/RGB).
Note: an earlier Arduino/PlatformIO plan (`FIRMWARE.md`, `Orb_Migration_Spec.md`) was abandoned — this is a pure ESP-IDF 5.5 + LVGL 8.4 build. Those docs are gone; ignore any reference to a PlatformIO/Arduino toolchain.
Module overview + diagnostics: [`README.md`](README.md).
OTA release runbook: [`DEPLOYMENT.md`](DEPLOYMENT.md) — read it when the user says "deploy this for OTA" or any equivalent.

## PTT — actual values in firmware (BEHAVIOR.md §3.1, with our pad reduction)
Live constants in `main/Convai/convai.c`:

- Frame: **30 ms = 480 samples = 960 bytes** (16 kHz, int16, MSB-aligned in 32-bit slot)
- Power-rail wait after press accepted: **150 ms** (`PTT_POWER_RAIL_WAIT_MS`)
- Silent-revert threshold: press **< 1000 ms** → no pad, no turn end (`PTT_PRESS_MIN_MS`)
- Tail silence pad on release: **43 × 30 ms ≈ 1290 ms** (`PTT_TAIL_SILENCE_FRAMES`)
  - Server VAD silence threshold is pinned to **800 ms** in `send_initiation()` (`turn_detection.silence_duration_ms`), giving ~490 ms margin.
  - Earlier value was 27 frames (810 ms) — landed right on the threshold and caused server to merge consecutive PTT presses into one user turn. Do not shorten without lowering the server threshold to match.
- Short-turn skip: real audio **< 800 ms** → treated same as silent revert (`PTT_SHORT_TURN_MIN_MS`)
- Button debounce: **50 ms** (`main/Button/button.c::DEBOUNCE_MS`)

## ElevenLabs WSS
Full reference: [CONVAI.md](CONVAI.md). The short-form rules below are the
load-bearing ones — anything more nuanced is in CONVAI.md.

- URL: `wss://api.elevenlabs.io/v1/convai/conversation?agent_id=<id>&inactivity_timeout=120`
- Auth: `xi-api-key` header (from `secrets.h::EL_API_KEY`)
- TLS root: pin `certs/gts_root_r1.pem` (built-in CA bundle didn't match on our IDF build)
- **Two keepalives**:
  - **`pong` reply on every server `ping`** — required. Sent inline from `process_message`, with one important gate: skipped while `s_ptt_held` is true (mic_task is hammering the WS lock with audio frames and pong contention would 50 ms-timeout the lib's send). Skipped pongs are counted in `s_pongs_skipped_ptt` and surfaced in the heartbeat; resume normally on PTT release.
  - **Periodic `user_activity` every 60 s** while WS is up and PTT is idle — proactive keepalive, matches `maintain_pong()` in the legacy Pi (`futuresGarden/main.py`). Without it, the server's ping cadence could drift past our `inactivity_timeout` and we die silently. Sent from `heartbeat_task`.
- WSS task stack: **6144 bytes** (`task_stack=6144` in `convai_start`). Was 8192 originally, but by the time `convai_start` runs the internal heap is fragmented by WiFi/lwIP/mbedTLS and the largest contiguous block is ~7.5 KB; 8 KB stack fails to allocate. 6 KB fits and still leaves ~2 KB margin over the WS handler's observed worst case (cJSON parse + 768 B audio chunk + base64 decode). Do not raise without checking `start: heap_int=N largest_int=M` probe at boot.
- Reassembly buffer: 2 MB PSRAM — agent audio events come in as single messages up to 250+ KB
- PCM ring: 128 KB PSRAM, drained by playback task on core 1
- Decode audio in 1 KB / 768 B base64 chunks with `portMAX_DELAY` backpressure — never accumulate a whole turn in RAM
- Persistent WSS across many user turns. Only `convai_stop()` closes it; called from NFC re-scan path.

### App-level send queue (the fix for "conversation restarts mid-PTT")
- **Why it exists**: previously `mic_task` called `esp_websocket_client_send_text` directly every 30 ms. On *any* transient upstream congestion (TCP send buffer full, brief WiFi blip, etc.), the send call returned ≤ 0. Within ~1-2 s of these fail-fast returns the WS lib concluded the transport was dead, fired `WEBSOCKET_EVENT_DISCONNECTED`, auto-reconnected, called `send_initiation()`, and the agent re-greeted. Three confirmed root causes were `transport_poll_write(0)`, `errno=104 (ECONNRESET)`, and WiFi STA reassociation — only the first is preventable.
- **Architecture** (all in `Convai/convai.c`):
  - `mic_task` and `send_silence_frames` enqueue JSON payloads via `convai_send_enqueue` instead of sending directly.
  - 1 MB `xRingbuffer` (`RINGBUF_TYPE_NOSPLIT`) with PSRAM-backed storage holds queued frames (~24 s of mic audio at 43 KB/s).
  - `sender_task` (pinned core 0, prio 5) pops one item at a time, sends it, retries the same item on failure with **500 ms backoff**. Never drops a frame.
  - **Producer never drops either** — `xRingbufferSend(..., portMAX_DELAY)` on queue-full. `mic_task` will block on enqueue if the link is genuinely down; mic capture pauses, no audio is lost. Counted in `s_queue_block_count`.
  - Direct sends (kept bypassing the queue): pong, `user_activity`, `send_initiation`. They're rare or one-shot — too infrequent to drive fail-fast.
- **Sender stack lives in PSRAM** via `xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)`. The internal SRAM is already fragmented by the time `convai_start` runs; trying to allocate the sender's 4 KB stack from internal heap fails silently (no `sender_task started` log appears). Keep it in PSRAM — a few µs context-switch overhead is irrelevant for this task.
- **On `WEBSOCKET_EVENT_DISCONNECTED`** the queue is drained (frames are for a dead session; replaying them against the reconnected session's fresh greeting context would confuse the server). `s_q_bytes_in` / `s_q_bytes_out` are reset.
- **Observability** (heartbeat verbose line):
  - `q_depth=NB` — current backlog. Healthy steady state is 0; non-zero during PTT only if sender is briefly behind.
  - `q_block=N` — cumulative times producer blocked because queue was full. Should stay 0 in normal operation.
  - `q_retry=N` — cumulative `send_text` failures. Stays 0 on a healthy link; rises during real congestion.
  - `sender: sent #N (item=NB, last_fail_streak=K)` every 33 successful sends. Confirms sender is alive and how often it had to retry.
- **Heap probe at WS start** (`start: heap_int=N largest_int=M psram=K before client_start`) is intentional — keep it. If `largest_int` ever drops below ~6500, the WS task spawn is at risk and we need to look at this again.
- **Don't reintroduce direct `send_text` calls in `mic_task`**. The whole fix is that the WS lib never sees a fail-fast pattern from us. A single direct send from the mic loop would re-introduce the bug.

## NFC
- Polling interval: 100 ms. Same-UID debounce: 1.5 s.
- NFC task stack: **8 KB minimum** (`orb_refresh_config()` does an HTTPS GET with TLS handshake)
- On every tag scan: `orb_refresh_config()` runs first so volume / agent changes from Supabase land without a reboot.
- Scan UI feedback (`nfc.c::handle_uid`): `orb_ui_set_state(ORB_NFC)` is set **before** the blopp tone so the purple "NFC Scanned" screen is already up while the cue plays. It is held through the TLS config fetch (don't set `ORB_LOADING` early — it would overwrite `ORB_NFC` instantly), then `ORB_LOADING` is set just before `convai_start()` for the WSS-connect phase; the Convai tasks take over from there.
- Re-scan during running session → `convai_stop()` → `orb_refresh_config()` → `convai_start()` (full restart). `ORB_NFC` stays up across the teardown wait.
- PN532 init failure: surface via the `errors=N` counter in `nfc.c::nfc_task`. (LCD error scene TBD — see PROGRESS.md.)
- Tag table per BEHAVIOR.md §6 (AGENT_START / TEST / custom-phrase) **not yet implemented** — today any tag is treated as AGENT_START.

## Audio (I2S)
- **Split I2S, not duplex**: I2S_NUM_0 TX master + I2S_NUM_1 RX slave on shared BCLK/WS pins. Duplex mode gates clocks when TX is disabled and the mic dies — don't merge them.
- TX channel runs continuously after `i2s_audio_init()`. Speaker silence during PTT is achieved via `dout_mute()` (GPIO matrix swap → DOUT driven low → MAX98357A auto-mutes ~25 ms). Don't disable the TX channel — RX shares its BCLK/WS.
- INMP441 SD on **GPIO 20** (NOT GPIO 0 — strap pin pull-up masks the open-drain output).
- INMP441 needs 32 BCLK / slot or it never drives data. Slot config: `slot_bit_width=32`, `ws_width=32`, `left_align=true`. Data: 16 kHz, mono, 32-bit data width (TX widens int16 by `<<16`, RX shifts int32 by `>>15` to add 1 bit of software gain).
- Volume mapping: Supabase `volume` (1..10) → `i2s_audio_set_volume_pct(vol * 10)`. Applied in `orb_refresh_config()`.

## GPIO 0 / PTT button
- Button between GPIO 0 and GND. External + internal strap pull-up holds the line HIGH when released.
- **Don't hold the button at boot** — GPIO 0 LOW at reset = ROM download mode, firmware never starts.
- Ground via the RTC battery header's GND pin (star-grounding) so contact-bounce currents don't ride the mic's GND wire.

## Battery sensing — full reference: [`BATTERY.md`](BATTERY.md)
- **GPIO4 ADC only. No INA219** (that was the Pi build) — voltage, no current.
- **ADC senses board voltage, NOT cell voltage** — it's downstream of the power switch + harness, so it reads ~20–190 mV *below* the cell, load-dependently. All gauge constants are anchored to that post-drop reading. Never recalibrate against cell voltage.
- Gauge in `orb_ui.c`: `BATTERY_MV_EMPTY 3550` (≈LDO brown-out edge) → 0 %, `BATTERY_MV_FULL 3930` (on-screen volts at full cell, unplugged, under load) → 100 %. Anchored to the unplugged-under-load reading so a full pack reads 100 % in every condition (charging clamps); tradeoff is it sticks at 100 % through the top ~15-20 % of discharge. Cosmetic only; real cutoff is the TP4056/DW01 at ~2.5 V cell.
- Displayed value is a **2 s moving average** — `BAT_AVG_WINDOW 20` × 100 ms loop, each sample a 16× ADC burst (`BAT_Driver.c`). Tune via `BAT_AVG_WINDOW` only.
- **Don't re-add idle-gated sampling** (BEHAVIOR.md §9.2). This firmware has no idle state — it boots straight into a persistent conversation; `ORB_SPLASH` is never set. Tried it, label went blank, reverted.
- Label uses **integer** voltage formatting (`%d.%02dV`) — LVGL's `%f` is disabled in the locked LV config, so `%.2f` renders as a stray "fV".

## LVGL + RGB panel — Travis is the source of truth (DO NOT DRIFT)

**Versions are locked. Never change them.** [`main/idf_component.yml`](main/idf_component.yml) pins:
- **ESP-IDF == 5.5.0**
- **LVGL == 8.4.0**

Full reference: [SCREEN.md](SCREEN.md). Reference codebase: [`travis_Leftscreen/`](travis_Leftscreen/) (vendored copy of `github.com/traviscea/right-side-cluster-esp32s3`) — it runs tear-free at high FPS on this exact Waveshare 2.8C panel. **Any change to LCD or LVGL driver code, or to screen-related sdkconfig, MUST be benchmarked against his code. Do not invent alternative configurations. Do not upgrade IDF or LVGL "to try something."**

If asked to bump either version: refuse, point at this section. The combo has cost weeks of debugging to land on. Bumping has broken it every time. If the user insists, demand they explicitly acknowledge the screen will probably tear again and they accept that risk.

Known-bad alternatives (all tried, all reverted, see [SCREEN.md](SCREEN.md) failed-approaches table):
- LVGL 9.x + esp_lvgl_port — tears in every config combo
- ESP-IDF 6.x — removed `psram_trans_align` from `esp_lcd_rgb_panel_config_t`
- `CONFIG_EXAMPLE_DOUBLE_FB=y` — tears between framebuffers
- Removing `CONFIG_EXAMPLE_USE_BOUNCE_BUFFER` — tears immediately
- `CONFIG_LCD_RGB_RESTART_IN_VSYNC=y` — doesn't help; not needed on this panel revision
- **Screen knobs (verbatim from Travis, captured in [sdkconfig.defaults](sdkconfig.defaults)):**
  - `CONFIG_EXAMPLE_USE_BOUNCE_BUFFER=y` — bounce mode through internal SRAM is what kills the tearing.
  - `CONFIG_EXAMPLE_AVOID_TEAR_EFFECT_WITH_SEM=y` — vsync semaphore coordination in [ST7701S.c::example_on_vsync_event](main/LCD_Driver/ST7701S.c).
  - Single framebuffer (`EXAMPLE_LCD_NUM_FB=1`). Do NOT enable `CONFIG_EXAMPLE_DOUBLE_FB`.
  - Do NOT enable `CONFIG_LCD_RGB_RESTART_IN_VSYNC` — Travis runs without it and there's no horizontal offset on this panel.
  - `CONFIG_LV_DISP_DEF_REFR_PERIOD=15`, `CONFIG_LV_INDEV_DEF_READ_PERIOD=15`, `CONFIG_FREERTOS_HZ=100`.
- **LVGL_Driver.c flush_cb** does NOT take/give the vsync sem. With bounce mode the panel DMA handles tearing; flush_cb just calls `esp_lcd_panel_draw_bitmap` and `lv_disp_flush_ready`.
- **Cross-task LVGL access:** wrap with `LVGL_Lock(timeout_ms)` / `LVGL_Unlock()` (thin mutex around the LVGL task; not from esp_lvgl_port).
- **v8 API quirks:** `lv_scr_act()` (not `lv_screen_active`), `lv_obj_clear_flag` (not `lv_obj_remove_flag`). `lv_obj_remove_style_all(screen)` is still needed to defeat default padding.
- If reintroducing screen tearing while making any change: revert and re-diff against `travis_Leftscreen/` before doing anything else.

## Logging (when USB is dead — basically always)
- Audio init kills both USB-C ports. Console = UDP from then on.
- `log_sink_start(6666)` runs after WiFi gets IP.
- On the laptop: `nc -ul 6666`. Use **Ctrl-C** to stop, not Ctrl-Z (Ctrl-Z leaves the port bound).
- If your AP throttles broadcast, set `LOG_SINK_TARGET_IP` in `main/Wireless/log_sink.c` to the listener's IP (currently `192.168.1.27`).
