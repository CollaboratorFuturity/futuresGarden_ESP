# J9 Audio Wiring — INMP441 + MAX98357A

| GPIO | Pin label | Connect to |
|------|-----------|------------|
| 43   | TXD       | INMP441 SCK + MAX98357A BCLK |
| 44   | RXD       | INMP441 WS + MAX98357A LRC |
| 19   | D-        | MAX98357A DIN |
| 20   | D+        | INMP441 SD |
| —    | 3V3       | INMP441 VDD  |
| -    | 5V.       | MAX98357A VIN
| —    | GND       | all breakouts |

MAX98357A configured for option A (1W, consistent output independent of battery state).
VBus (5V) not used. MAX98357A SD pin is left unconnected (internal 1 MΩ pull-up keeps
the amp enabled in Left-channel mode; we don't drive it from a GPIO).

## PTT button — separate from J9

PTT now lives on **GPIO 0 → GND**, grounded via the RTC battery header's GND pin
(star-grounding). It is NOT on J9. See `main/Button/button.c` for debounce and edge
handling.

## Pin choice notes

- **INMP441 SD on GPIO 20** (was originally GPIO 0). GPIO 0 is a boot-strapping
  pin with strong external pull-up + boot button on the dev board, which masks
  the INMP441's open-drain SD output: silence reads as exact zeros and loud
  audio reads as bit-slipped garbage with rail-saturated samples. GPIO 20 has
  no strap circuitry and works cleanly. The PTT button that used to live on
  GPIO 20 has been moved to GPIO 0 (whose strap pull-up is *good* for a button
  but *bad* for a CMOS-driven data line).
- **INMP441 L/R pin** must be tied to GND (LEFT slot output). Floating it
  causes the chip to output unpredictably.
- **GPIO 19/20/43/44** are the native USB-CDC / UART-bridge pins; once
  `i2s_audio_init()` runs, both USB ports go dark for the rest of the
  session. Logging falls back to the UDP sink (see `Wireless/log_sink.c`).

## I2S configuration notes (driver-side)

- The firmware uses **split I2S**, not duplex: I2S_NUM_0 is TX master (drives
  BCLK/WS/DOUT), I2S_NUM_1 is RX slave (reads BCLK/WS as inputs from the same
  physical pins via GPIO matrix). Duplex on one peripheral gates the clocks
  when TX is disabled, which kills the mic — see `main/Audio/i2s_audio.c`.
- **32-bit slot** even though we use 16-bit data: INMP441 needs 32 BCLK / slot
  or it never drives data. Slot config: `slot_bit_width=32`, `ws_width=32`,
  `left_align=true`, Philips.
- **Speaker mute during PTT** is done by detaching the DOUT pin from the I2S
  signal via the GPIO matrix (`dout_mute()` in `i2s_audio.c`) and driving it
  LOW from GPIO. The MAX98357A auto-mutes ~25 ms later. The TX channel itself
  stays running so RX (the mic) keeps its clocks.
