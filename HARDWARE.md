# Futures Garden — Hardware Reference (ESP32-only build)

Single source of truth for the electronics and wiring of the **ESP32-only**
conversational orb (`futuresGarden_ESP32/LVGLastest_ESP-IDF`). Everything below
was read out of the firmware drivers and `sdkconfig`, so it reflects what the
code actually expects — if you rewire, change it here and in the driver header
named in each row.

> Sibling project note: the Raspberry-Pi-paired build (`futuresGarden/`) runs
> only the UI on its ESP32 and takes commands over serial. This document covers
> the **standalone** board, where the ESP32-S3 runs everything (Wi-Fi, audio,
> NFC, ConvAI, UI).

---

## 1. Module & board

| Item | Value |
|---|---|
| MCU | **ESP32-S3** (Xtensa LX7 dual-core, 240 MHz) |
| Board | Waveshare **ESP32-S3-Touch-LCD-2.8C** (480×480 round panel) |
| PSRAM | 8 MB **octal** (OPI), 80 MHz — `SPIRAM_MODE_OCT`, type auto-detected |
| Flash | **16 MB** (`ESPTOOLPY_FLASHSIZE_16MB`) |
| Partition table | custom (`PARTITION_TABLE_CUSTOM`) |
| FreeRTOS tick | 100 Hz |

ConvAI/TLS notes baked into `sdkconfig.defaults`: mbedTLS allocations are routed
to PSRAM (`MBEDTLS_EXTERNAL_MEM_ALLOC`) and hardware AES is **disabled**
(`MBEDTLS_HARDWARE_AES=n`) because the DMA-capable SRAM the HW accelerator needs
gets exhausted by LVGL + audio + the websocket task during agent audio.

---

## 2. Display — ST7701S, 480×480 RGB parallel

16-bit RGB565 over the S3 RGB LCD peripheral. 18 MHz pixel clock, single
framebuffer in PSRAM, bounce-buffer + vsync-end semaphore for tear-free output.
Defined in `main/LCD_Driver/ST7701S.h`. Touch (GT911) is physically present but
**not used** by this firmware — input comes from the button instead.

| Signal | GPIO | | Signal | GPIO |
|---|---|---|---|---|
| HSYNC | 38 | | DE | 40 |
| VSYNC | 39 | | PCLK | 41 |
| Backlight (LEDC PWM, active-HIGH) | 6 | | DISP_EN | n/c (-1) |

**RGB data lines**

| Blue | GPIO | | Green | GPIO | | Red | GPIO |
|---|---|---|---|---|---|---|---|
| B0 | 5 | | G0 | 14 | | R0 | 46 |
| B1 | 45 | | G1 | 13 | | R1 | 3 |
| B2 | 48 | | G2 | 12 | | R2 | 8 |
| B3 | 47 | | G3 | 11 | | R3 | 18 |
| B4 | 21 | | G4 | 10 | | R4 | 17 |
| | | | G5 | 9 | | | |

LVGL refresh / input read period: 15 ms (`LV_DISP_DEF_REFR_PERIOD`). Fonts
enabled: Montserrat 16/20/22/24/26.

---

## 3. I²C bus (shared)

One master bus, defined in `main/I2C_Driver/I2C_Driver.h`. 400 kHz.

| Signal | GPIO |
|---|---|
| SCL | 7 |
| SDA | 15 |

**Devices on the bus**

| Device | Address | Role | Driver |
|---|---|---|---|
| TCA9554PWR IO expander | `0x20` | Panel reset / control lines | `main/EXIO/TCA9554PWR.*` |
| PN532 NFC reader | `0x24` | Tag scanning (`ORB_NFC`) | `main/NFC/nfc.c` |
| GT911 touch controller | (present) | **Unused** in this build | — |

The TCA9554 expander provides 8 extra IO lines (EXIO1–EXIO8) used for panel
reset and related control that aren't wired to native GPIO.

---

## 4. Audio — I²S

INMP441 mic in, MAX98357A amp out. The single S3 I2S peripheral is split:
I2S_NUM_0 = TX master (drives BCLK/WS/DOUT), I2S_NUM_1 = RX slave (reads the same
BCLK/WS, plus DIN). 16 kHz, 30 ms frames (480 samples). Defined in
`main/Audio/i2s_audio.c`.

| Signal | GPIO | Destination |
|---|---|---|
| BCLK | 43 | shared (amp + mic) |
| WS / LRCLK | 44 | shared (amp + mic) |
| DOUT | 19 | → MAX98357A DIN (speaker) |
| DIN | 20 | ← INMP441 SD (mic) |

> Mic DIN was deliberately moved off GPIO 0 to avoid strap-pin contention.

---

## 5. Button

| Signal | GPIO | Notes |
|---|---|---|
| Push button (push-to-talk) | 0 | Active-LOW (LOW = pressed). Shared with the **BOOT strap pin**. Driver: `main/Button/button.c` |

---

## 6. Battery monitoring

ADC1 one-shot with calibration, in `main/BAT_Driver/BAT_Driver.*`.

| Item | Value |
|---|---|
| ADC | ADC1, channel 3 = **GPIO4** |
| Attenuation | 12 dB |
| Scaling | `volts = cali_mV × 3.0 / 1000 / Measurement_offset` (≈3× external divider) |
| Exposed as | `float BAT_analogVolts`, `BAT_Get_Volts()` |

Feeds the UI via `orb_ui_set_battery(float volts)`, which maps 3.0 V → 0 % and
4.17 V → 100 % for the battery label.

---

## 7. SD card — SDMMC (1-bit)

Defined in `main/SD_Card/SD_MMC.h`.

| Signal | GPIO |
|---|---|
| CLK | 2 |
| CMD | 1 |
| D0 | 42 |
| D1 / D2 / D3 | unused (-1) |

---

## 8. GPIO map (quick lookup)

| GPIO | Function | | GPIO | Function |
|---|---|---|---|---|
| 0 | Button / BOOT strap | | 21 | LCD B4 |
| 1 | SD CMD | | 38 | LCD HSYNC |
| 2 | SD CLK | | 39 | LCD VSYNC |
| 3 | LCD R1 | | 40 | LCD DE |
| 4 | Battery ADC (ADC1_CH3) | | 41 | LCD PCLK |
| 5 | LCD B0 | | 42 | SD D0 |
| 6 | LCD backlight (PWM) | | 43 | I2S BCLK |
| 7 | I²C SCL | | 44 | I2S WS |
| 8 | LCD R2 | | 45 | LCD B1 |
| 9 | LCD G5 | | 46 | LCD R0 |
| 10 | LCD G4 | | 47 | LCD B3 |
| 11 | LCD G3 | | 48 | LCD B2 |
| 12 | LCD G2 | | 17 | LCD R4 |
| 13 | LCD G1 | | 18 | LCD R3 |
| 14 | LCD G0 | | 19 | I2S DOUT |
| 15 | I²C SDA | | 20 | I2S DIN |

---

## 9. Strap-pin cautions

- **GPIO 0** doubles as the button and the BOOT strap. Hold it during reset only
  if you intend to enter download mode.
- LCD data lines on **45, 46, 47, 48, 3, 8** sit on flash/strap-adjacent pins;
  panel init order matters, which is why the ST7701S bring-up and the RGB
  peripheral start are sequenced the way they are in the driver.
- Mic DIN was relocated to **GPIO 20** specifically to keep GPIO 0 free of I2S
  contention.

---

*Source: read from `main/**` drivers and `sdkconfig` / `sdkconfig.defaults`.
Keep this file in sync when pins or peripherals change.*
