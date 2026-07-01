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
- **Three convai task stacks live in PSRAM** via `xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)`: `sender_task` (4 KB), `mic_task` (4 KB), `heartbeat_task` (3 KB). Only `playback_task` (4 KB) and the WS lib's own task (6 KB) remain in internal SRAM. By the time `convai_start` runs, internal heap is fragmented enough that 4 KB allocations may fit ONCE (playback) but the next one fails — silently, no panic. When `mic_task` failed to spawn, PTT did nothing (button events fired but no audio was sent) and the only signal was the `start: mic_task spawn FAILED` log line. Keep these three in PSRAM — a few µs context-switch overhead is irrelevant; the alternative is silent feature loss.
- **On `WEBSOCKET_EVENT_DISCONNECTED`** the queue is drained (frames are for a dead session; replaying them against the reconnected session's fresh greeting context would confuse the server). `s_q_bytes_in` / `s_q_bytes_out` are reset.
- **Observability** (heartbeat verbose line):
  - `q_depth=NB` — current backlog. Healthy steady state is 0; non-zero during PTT only if sender is briefly behind.
  - `q_block=N` — cumulative times producer blocked because queue was full. Should stay 0 in normal operation.
  - `q_retry=N` — cumulative `send_text` failures. Stays 0 on a healthy link; rises during real congestion.
  - `sender: sent #N (item=NB, last_fail_streak=K)` every 33 successful sends. Confirms sender is alive and how often it had to retry.
- **Heap probe at WS start** (`start: heap_int=N largest_int=M psram=K before client_start`) is intentional — keep it. Confirmed baseline after OTA changes: `heap_int≈22 KB, largest_int≈7.6 KB`. The WS task takes that largest block; everything after has to fit in the next-largest fragment. That's why three of the four convai tasks live in PSRAM (above). If `largest_int` ever drops below ~6500, the WS task itself is at risk — investigate before doing anything else.
- **Don't reintroduce direct `send_text` calls in `mic_task`**. The whole fix is that the WS lib never sees a fail-fast pattern from us. A single direct send from the mic loop would re-introduce the bug.

## OTA — boot-time GitHub release pull
Release runbook: [`DEPLOYMENT.md`](DEPLOYMENT.md). Implementation: [`main/OTA/ota.c`](main/OTA/ota.c).

- **Partitions** ([`partitions.csv`](partitions.csv)): `nvs (24K)` · `otadata (8K)` · `ota_0 (4M)` · `ota_1 (4M)`. App is ~1.5 MB today → 63% slot free. **No factory partition** — both slots are OTA.
- **Boot order in `post_connect_task`** (load-bearing):
  1. `log_sink_start(6666)` — UDP comes up FIRST so a crash in any later step is visible (USB-CDC is already dead by this point — `i2s_audio_init` killed it).
  2. `orb_sntp_sync` — TLS cert validation needs the clock right.
  3. `orb_ota_check_and_update_on_boot()` — **before** Supabase config fetch and convai_start.
  4. `orb_refresh_config()` then `convai_start()`.
- **Why OTA runs before config + convai**: OTA is the recovery floor. A broken release that crashes in `orb_refresh_config` or `convai_start` is unrecoverable if OTA is gated on those succeeding. As long as the orb gets to WiFi + DNS + TLS to GitHub, a bad firmware can be replaced.
- **Version compare**: `CONFIG_APP_PROJECT_VER_FROM_GIT_DESCRIBE=y` bakes `git describe` (e.g. `v0.0.2`) into `esp_app_get_description()->version`. OTA fetches `https://api.github.com/repos/<OWNER>/<REPO>/releases/latest`, parses `tag_name`, compares with `strcmp != 0`. **Any** difference triggers download. Make sure you tag BEFORE building the release binary or the embedded version will be a junk `1-NOTAG-gXXXX` string.
- **Dev-build guard**: because the compare is a plain `strcmp != 0` (no "is it newer?" check), a locally-flashed build whose version differs from the latest release would be auto-**downgraded** to that release on boot — clobbering the local image. So `orb_ota_check_and_update_on_boot()` **skips OTA entirely** when the running version looks like a dev build: contains `-dirty`, `-g<hash>` (git-describe "commits ahead" marker), or `NOTAG`. Only clean tagged releases (version == bare tag) participate in auto-update. Don't remove this guard or every dev flash gets pulled back to the published release.
  - **Toggle to test OTA on a dev build**: `CONFIG_ORB_OTA_UPDATE_DEV_BUILDS` (Kconfig in `main/Kconfig.projbuild`, default `n`) compiles the guard out when `=y`, so a dev-versioned image *will* take OTA. Enable via `idf.py menuconfig` — it lands in the gitignored `sdkconfig`, never `sdkconfig.defaults`, so it can't leak into a release. **No-op for clean releases** (their version has no `-g`/`-dirty`/`NOTAG`), so it only ever affects dev images on the bench. Runbook: [DEPLOYMENT.md](DEPLOYMENT.md).
- **TLS**: bundle (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`) handles both `api.github.com` and the 302 redirect target `objects.githubusercontent.com`. Don't try to pin per-host — GitHub release downloads cross hosts.
- **Download buffers**: the asset GET 302-redirects from `github.com` to a **signed `objects.githubusercontent.com` URL with a 600-900+ char query string**. The default 512 B HTTP client buffers can't hold that request line → `HTTP_CLIENT: Out of buffer` → `esp_https_ota` fails before the GET is sent. The download `http_config` sets `buffer_size = buffer_size_tx = 4096` to fit it. Symptom if reverted: OTA logs `update available … downloading`, three `Certificate validated` lines, then `Out of buffer`.
- **Releases body buffer goes on the HEAP, not the stack** — see big comment in `fetch_latest_release()`. 8 KB ctx + ~6 KB mbedTLS handshake on an 8 KB task stack overflows silently (no panic dump, just immediate reset). Symptom was: device logs `ota: GET https://...` then reboots. Same trap awaits anyone adding a second "fetch JSON from cloud" path.
- **`post_connect_task` stack: 8192 bytes** (was 6144 originally for one TLS handshake). Two back-to-back handshakes (Supabase + GitHub) need the margin. Bigger stack steals internal SRAM that convai tasks would otherwise use — see PSRAM-tasks note above.
- **Rollback safety**: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. New images boot in `PENDING_VERIFY`. `orb_ota_mark_running_valid()` is called from the `WEBSOCKET_EVENT_CONNECTED` handler in `convai.c` — once WSS connects, we know WiFi/TLS/auth all work, mark the image valid. If a new build crashes before WSS, the bootloader reverts on the next power cycle.
- **Repo coordinates** live in `secrets.h` as `OTA_GITHUB_OWNER` / `OTA_GITHUB_REPO`. Must point at a PUBLIC repo (anonymous request, no token). Currently `CollaboratorFuturity/futuresGarden_ESP`.

## Per-orb identity (device_id) — lives in NVS, survives OTA
Each orb fetches its Supabase config keyed on a `device_id`. **The id lives in NVS, NOT the firmware image** (`main/Wireless/config_fetch.c::orb_device_id`). This is load-bearing: OTA ships ONE binary to every orb, so a baked-in id would repaint the whole fleet on every update. Provisioning runbook: [DEPLOYMENT.md](DEPLOYMENT.md).
- **`DEVICE_ID` in `secrets.h` is a one-time provisioning *seed*, not the live id.** Resolution order in `orb_device_id()`: (1) `DEVICE_ID` set **and** `DEVICE_ID_OVERWRITE=1` → force-rewrite NVS (deliberate re-provision); (2) NVS has a `device_id` → use it (normal, OTA-safe path); (3) NVS empty → seed once from `DEVICE_ID` (or a `orb-XXXXXX` MAC id if undefined), persist, use it.
- **Provision per orb with one cable flash**: set `#define DEVICE_ID "<color>"`, build from a clean release tag, flash. First boot logs `device_id=<color> (seeded into NVS on first boot)` and stamps NVS. `secrets.h` is gitignored, so changing the color between orbs doesn't change the version — every orb stays a clean tagged build on the OTA track.
- **Published release assets must be built NEUTRAL** — `DEVICE_ID` commented out — so an unprovisioned orb that auto-pulls a release seeds a harmless MAC id, never a color. Verify: `strings build/…bin | grep -iE '^(black|purple|…)$'` → no match.
- **Re-provision** a stamped orb with `idf.py erase-flash` (wipes NVS → re-seeds) or `DEVICE_ID_OVERWRITE 1` (no-erase rewrite; never ship a release with it =1).
- **Migration trap**: you can't roll identity out via one shared OTA release — every unprovisioned orb would seed from that release's baked id at once. Provision by cable. A live orb already on the OTA track auto-pulls a new neutral release and seeds a MAC id, losing its friendly config until cable-provisioned.

## WiFi — multi-network, connect to first in-range
- **Networks come from a priority LIST, not a single SSID.** `secrets.h` defines `WIFI_CREDS` — an array of `{ "ssid", "password" }` in priority order (top = highest). The old `WIFI_SELECT` single-pick macro is gone; don't reintroduce it.
- **`wifi_connect_best()` (`main/Wireless/Wireless.c`)** scans on `STA_START` and after every `STA_DISCONNECTED`, then associates to the **first listed network that's actually in range**. `WIFI_Init` only starts STA now — per-network `esp_wifi_set_config` happens in `wifi_connect_best()`, not at init. Don't move config back into `WIFI_Init`.
- **Selection is list-order priority, not signal strength.** Among visible networks the highest in the list wins. (RSSI preference would be a one-line change on `recs[r].rssi`.)
- **Failure handling**: an absent network is skipped instantly (scan-based, no timeout). A network that's present but rejects auth (wrong password: reason 202/15/204/200) is flagged in `s_auth_failed[]` and skipped for the rest of the boot so it can't loop; if *every* cred auth-fails the flags reset rather than lock the orb out. `NO_AP_FOUND` (201) is just out-of-range and stays eligible for the next scan.
- **Roaming implication**: an orb moves between any listed networks with no reflash. Adding/removing networks for the fleet is a compile-time change → new release (creds are baked from the gitignored `secrets.h`). WiFi-change-over-OTA rules + rollout in [DEPLOYMENT.md](DEPLOYMENT.md).

## NFC
- Polling interval: 100 ms. Same-UID debounce: 1.5 s.
- NFC task stack: **8 KB minimum**. The `handle_uid` path can do **two sequential** HTTPS GETs with TLS handshakes — the tag-table download (`orb_nfc_tags_fetch()`) and `orb_refresh_config()`. They run one after another, so peak stack is one handshake at a time; 8 KB holds. Don't drop below it.
- **Tag table is implemented** (BEHAVIOR.md §6) and **downloaded from GitHub each session** — `NFC_TAGS_URL` in `secrets.h` (raw file on the public OTA repo), fetched via the IDF cert bundle in `config_fetch.c::orb_nfc_tags_fetch()` into a 32 KB PSRAM buffer. **No embed, no NVS cache, no offline fallback** — the orb needs internet to converse at all, so an offline-only table would never run. The parsed `cJSON *s_tags` is owned solely by `nfc_task` (warmed before the poll loop, lazily reloaded on the first scan if the network was late, reloaded on every AGENT_START/TEST), so it needs no lock.
- **UID format is colon-hex** (`uid_to_hex` emits `04:38:17:9A:CB:2A:81`) to match the JSON keys. This was a real bug: the old formatter emitted colon-less hex, which would miss every lookup. Don't revert it.
- **Dispatch in `handle_uid`** (lookup happens *before* any UI/tone so unmapped tags are a true no-op):
  - `AGENT_START` / `TEST` (reserved) → reload tags + `orb_refresh_config()` → restart session (`convai_stop` → `convai_start`). No splash/temp-WSS state exists here, so both reserved tags mean "reload + restart".
  - Any other mapped phrase, session running → **inject** `{"type":"user_message","text":"<phrase>"}` into the **live** WSS via `convai_send_user_message()` — no teardown, no restart. This is the point of the tag table. **The inject MUST be followed by a turn-end** — `convai_send_user_message()` appends the PTT tail-silence pad after the text, because the server uses audio-VAD turn detection (`turn_detection.silence_duration_ms`) and a text-only event never closes the turn → agent stays silent → UI stuck on `ORB_LOADING`. This is the ESP32 `force_turn_end` (BEHAVIOR.md §6.2/§3.5); it fires unconditionally since NFC scans happen with PTT idle. Verified on hardware. Don't strip the silence pad.
  - Mapped phrase, session not running (rare) → fall back to restart; phrase dropped.
  - **Unmapped UID → ignored** (no tone, no state change), per BEHAVIOR.md §6.
- **NFC scanning is gated to the idle window only.** `orb_ui_set_state()` (`orb_ui.c`) calls `NFC_Set_Polling(s == ORB_MUTED)`, so reads are allowed only between turns and suppressed during `ORB_USER_TALK` (PTT held), `ORB_LOADING` (awaiting agent), and `ORB_AGENT` (agent speaking). This is **stricter than BEHAVIOR.md §6.4**, which keeps NFC live during the agent response — on this build it's off while the agent talks too. `orb_ui_set_state` is the single choke point all modules route transitions through; don't scatter `NFC_Set_Polling` calls elsewhere. The poll loop calls `handle_uid` synchronously, so a scan that sets `ORB_LOADING`/`ORB_AGENT` naturally stops further scanning until `ORB_MUTED` returns.
- On every tag scan: `orb_refresh_config()` runs first so volume / agent changes from Supabase land without a reboot.
- Scan UI feedback (`nfc.c::handle_uid`): `orb_ui_set_state(ORB_NFC)` is set **before** the blopp tone so the purple "NFC Scanned" screen is already up while the cue plays. It is held through the TLS config fetch (don't set `ORB_LOADING` early — it would overwrite `ORB_NFC` instantly), then `ORB_LOADING` is set just before `convai_start()` for the WSS-connect phase; the Convai tasks take over from there.
- Re-scan during running session (AGENT_START/TEST) → `convai_stop()` → `orb_refresh_config()` → `convai_start()` (full restart). `ORB_NFC` stays up across the teardown wait.
- PN532 init failure: surface via the `errors=N` counter in `nfc.c::nfc_task`. (LCD error scene TBD — see PROGRESS.md.)

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
