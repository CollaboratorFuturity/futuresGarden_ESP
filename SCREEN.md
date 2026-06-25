# Screen stack — full reference

This document captures the **only known tear-free configuration** for the
Waveshare ESP32-S3-Touch-LCD-2.8C panel after weeks of trial and error.
The configuration is a verbatim copy of Travis's
[`right-side-cluster-esp32s3`](https://github.com/traviscea/right-side-cluster-esp32s3)
repo, vendored at [`travis_Leftscreen/`](travis_Leftscreen/) for offline diffing.

**Rule zero:** if you change anything in this stack and tearing reappears,
revert and `diff` against `travis_Leftscreen/` before doing anything else.
Do not invent alternative configurations. Do not upgrade LVGL or ESP-IDF
"to try something." This combo works; everything else we've tried doesn't.

---

## Hardware

- **Board:** Waveshare ESP32-S3-Touch-LCD-2.8C (16 MB flash, 8 MB octal PSRAM).
- **Panel:** ST7701S, 480×480 round, RGB565 16-bit parallel interface.
- **SPI init bus:** GPIO 1 (MOSI) / GPIO 2 (SCLK) / CS via TCA9554PWR I/O
  expander. SPI is only used for the init command sequence; pixel data
  goes over the parallel RGB bus.
- **RGB pins:** see `EXAMPLE_PIN_NUM_*` in
  [main/LCD_Driver/ST7701S.h](main/LCD_Driver/ST7701S.h) (HSYNC=38, VSYNC=39,
  DE=40, PCLK=41, D0..D15 across 5/45/48/47/21/14/13/12/11/10/9/46/3/8/18/17).
- **PCLK:** 18 MHz (`EXAMPLE_LCD_PIXEL_CLOCK_HZ`).
- **Backlight:** LEDC on GPIO 6, 13-bit resolution.

---

## Toolchain + dependency versions (load-bearing)

| Component | Version | Why |
|---|---|---|
| ESP-IDF | **5.5** | v6 removed `psram_trans_align` from `esp_lcd_rgb_panel_config_t` ([ST7701S.c:441](main/LCD_Driver/ST7701S.c#L441)). Travis was built on v5.x. |
| LVGL | **~8.4** (managed component `lvgl/lvgl`) | v9 + esp_lvgl_port + this panel = tearing in every variant we tried (direct_mode/full_refresh, bb_mode on/off, all FB combos). |
| esp_websocket_client | `1.4.0` (pinned exact) | Unrelated to screen, but newer versions call `esp_transport_ws_get_redir_uri` which isn't in IDF 5.5. |

The `esp_lvgl_port` component is **not** used — Travis registers the LVGL
display driver directly, and so do we.

---

## The tear-free config (sdkconfig.defaults)

Live file: [sdkconfig.defaults](sdkconfig.defaults). The screen-relevant knobs:

```
CONFIG_EXAMPLE_USE_BOUNCE_BUFFER=y          # ← the actual tearing fix
CONFIG_EXAMPLE_AVOID_TEAR_EFFECT_WITH_SEM=y # ← vsync coordination
# (no CONFIG_EXAMPLE_DOUBLE_FB — single framebuffer only)
# (no CONFIG_LCD_RGB_RESTART_IN_VSYNC — not needed on this panel)

CONFIG_LV_DISP_DEF_REFR_PERIOD=15           # ~66 Hz LVGL refresh
CONFIG_LV_INDEV_DEF_READ_PERIOD=15
CONFIG_FREERTOS_HZ=100
```

PSRAM (from [sdkconfig.defaults.esp32s3](sdkconfig.defaults.esp32s3)):

```
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
```

LVGL fonts our UI needs (Travis's project doesn't):

```
CONFIG_LV_FONT_MONTSERRAT_16=y
CONFIG_LV_FONT_MONTSERRAT_20=y
CONFIG_LV_FONT_MONTSERRAT_22=y
CONFIG_LV_FONT_MONTSERRAT_24=y
CONFIG_LV_FONT_MONTSERRAT_26=y
```

If you wipe `sdkconfig` to force defaults to reload, also run
`idf.py set-target esp32s3` afterwards (the target defaults back to `esp32`).

---

## How the tear-free combo actually works

`EXAMPLE_USE_BOUNCE_BUFFER=y` puts the RGB driver into bounce-buffer mode:

1. Single framebuffer lives in PSRAM (`fb_in_psram=true`).
2. The driver allocates a small **bounce buffer in internal SRAM**
   (`bounce_buffer_size_px = 10 * EXAMPLE_LCD_H_RES = 9600 px = 19200 B`).
3. EDMA copies framebuffer → bounce buffer just ahead of the panel's
   scan-out. The panel reads from the bounce buffer, not the FB.
4. LVGL writes new pixels into the framebuffer at any time. The panel
   never reads partially-written FB regions, because it's reading the
   bounce buffer instead.

This is why we don't need `direct_mode` / `full_refresh` / `avoid_tearing`
in LVGL — the tearing avoidance happens one level below LVGL, in the RGB
driver itself.

`EXAMPLE_AVOID_TEAR_EFFECT_WITH_SEM=y` adds vsync semaphore coordination
in [ST7701S.c::example_on_vsync_event](main/LCD_Driver/ST7701S.c) — on
every vsync IRQ, if `sem_gui_ready` is taken, `sem_vsync_end` is given.
Travis's `flush_cb` does NOT wait on these semaphores, and neither does
ours — bounce mode handles tearing fine on its own. The semaphore is set
up but unused on the LVGL side. Don't remove it; leave the wiring intact
to keep parity with Travis.

---

## Files in the screen stack

| File | Owner | Purpose |
|---|---|---|
| [main/LCD_Driver/ST7701S.h](main/LCD_Driver/ST7701S.h) | Travis (+ FreeRTOS.h include for IDF 5.5) | Pin defines, init command sequence prototypes, framebuffer count macros, semaphore externs. |
| [main/LCD_Driver/ST7701S.c](main/LCD_Driver/ST7701S.c) | Travis (+ minor: `Set_Backlight()` call uncommented) | ST7701S init over SPI, RGB panel allocation (`esp_lcd_new_rgb_panel`), vsync event callback, backlight LEDC. |
| [main/LVGL_Driver/LVGL_Driver.c](main/LVGL_Driver/LVGL_Driver.c) | Port of Travis's (touch stripped, lock/unlock wrapper added) | `lv_init`, draw-buffer alloc (60 lines in internal SRAM with `MALLOC_CAP_DMA \| MALLOC_CAP_INTERNAL`), flush_cb that calls `esp_lcd_panel_draw_bitmap`, 1 ms tick timer, `lvgl_task` pinned to core 1. |
| [main/LVGL_Driver/LVGL_Driver.h](main/LVGL_Driver/LVGL_Driver.h) | Ours | Externs + `LVGL_Lock()` / `LVGL_Unlock()` prototypes. |
| [main/LVGL_UI/orb_ui.c](main/LVGL_UI/orb_ui.c) | Ours | Application UI on top of the driver — central circle, agent name, state label, battery, 3 cycling progress rings. |
| [travis_Leftscreen/](travis_Leftscreen/) | Travis (vendored reference, do not modify) | Source of truth for diffs. |

---

## Boot / init sequence

From [main/main.c::app_main](main/main.c):

```
Driver_Init()                                # I2C + EXIO (TCA9554) up first
LCD_Init()                                   # ST7701S init seq + esp_lcd_new_rgb_panel
SD_Init()
LVGL_Init()                                  # lv_init, draw buffers, register driver,
                                             #   start lvgl_task on core 1
LVGL_Lock(0); orb_ui_init(); LVGL_Unlock();  # build widgets
NFC_Init()
button_init(on_button_state)
i2s_audio_init()                             # ⚠ kills USB-CDC + UART console
Wireless_Init()
```

`LCD_Init` must run before `LVGL_Init` (LVGL pulls the framebuffer
pointers from the panel handle when `EXAMPLE_DOUBLE_FB` is set — not our
case, but the order is still important because `LVGL_Driver.c` references
`panel_handle` exported from `ST7701S.c`).

---

## Cross-task UI updates

The LVGL task runs continuously in `lvgl_task()` ([LVGL_Driver.c](main/LVGL_Driver/LVGL_Driver.c)).
Anything touching LVGL widgets from another task **must** hold the LVGL
mutex:

```c
if (LVGL_Lock(0)) {        // 0 = wait forever
    lv_label_set_text(label, "foo");
    LVGL_Unlock();
}
```

For high-frequency updates (battery, agent state), the better pattern
is what `orb_ui.c` already does: write to an atomic from the background
task, then have a 50 ms LVGL timer (`orb_tick_cb`) read the atomic on
the LVGL task and apply the change — no mutex needed.

---

## Failed approaches (do not re-try)

Each of these was tried, made tearing worse or kept it, and was reverted.
If you find yourself reaching for one of these, stop and re-diff against
`travis_Leftscreen/` first.

| Tried | Result |
|---|---|
| LVGL 9.x + esp_lvgl_port | Tearing in every config combination: `direct_mode` vs `full_refresh`, `bb_mode` on/off, `avoid_tearing` on/off, `LV_DEF_REFR_PERIOD` 16/33/50. |
| `CONFIG_EXAMPLE_DOUBLE_FB=y` (2 framebuffers in PSRAM) | Tearing between FBs visible on animation. Travis runs single-FB. |
| Removing `CONFIG_EXAMPLE_USE_BOUNCE_BUFFER` ("the FB is in PSRAM, why need a bounce buffer?") | Tearing returns immediately. The bounce buffer in internal SRAM is what isolates the panel scan-out from PSRAM access latency. |
| `CONFIG_LCD_RGB_RESTART_IN_VSYNC=y` ("required for horizontal offset bug") | Doesn't help tearing. Travis runs without it and we see no horizontal offset on reset either. The horizontal-offset bug must be a different panel revision. |
| `-O2` (`COMPILER_OPTIMIZATION_PERF`) | No measurable benefit for tearing. Travis runs `-O0`. |
| Removing all UI animations | The screen is tear-prone with no animations either (any static frame still showed micro-tearing during state changes). The fix is in the driver/config, not the UI. |
| Polling battery voltage every 100 ms with re-render on every mv change | Hammers the screen with redraws as ADC noise jitters the reading. Not the cause of *tearing*, but a contributor to flicker. Throttle in the UI tick if it becomes a problem. |

---

## Diagnostics

- **LVGL perf monitor** — `CONFIG_LV_USE_PERF_MONITOR=y` shows FPS + CPU
  load overlay in the bottom-right corner.
- **`lvgl_task` is pinned to core 1.** WiFi/BT live on core 0. If you see
  the UI hitch when WiFi reconnects, that's expected core-0 contention,
  not a screen-driver bug.
- **If tearing reappears** after a code change:
  1. `diff main/LCD_Driver/ST7701S.c travis_Leftscreen/main/LCD_Driver/ST7701S.c`
  2. `diff main/LVGL_Driver/LVGL_Driver.c travis_Leftscreen/main/LVGL_Driver/LVGL_Driver.c`
  3. `diff <(grep -E '^CONFIG_(EXAMPLE|LV_|SPIRAM|LCD_RGB)' sdkconfig | sort) <(grep -E '^CONFIG_(EXAMPLE|LV_|SPIRAM|LCD_RGB)' travis_Leftscreen/sdkconfig | sort)`

  Anything that's not in Travis's repo is suspect.
