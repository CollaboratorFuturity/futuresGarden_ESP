# Futures Garden — Power + Audio Wiring (standalone build)

Hand-wiring instructions for the battery / charge / boost / audio subsystem of
the ESP32-only orb. The ESP32-S3 dev board's internal LCD/touch/SD wiring is
factory and not covered here (see [HARDWARE.md](HARDWARE.md)). This document
covers only the parts **you** solder: the power path and the two audio
breakouts.

Schematic: [`POWER_AUDIO_SCHEMATIC.svg`](POWER_AUDIO_SCHEMATIC.svg)

> Key design facts driving this wiring:
> - The board's **5 V pin is dead on battery** (only live over USB). The
>   MAX98357A must NOT take VIN from it.
> - The amp gets 5 V from a **dedicated boost** fed off the battery rail, so its
>   current spikes never sag the logic rail.
> - One **common ground** ties everything together. This is mandatory for the
>   I²S signals to have a return reference.

---

## 1. Bill of materials

| Ref | Part | Notes |
|---|---|---|
| BT1 | LiPo cell, 3.7 V 3500 mAh (SparkFun, w/ protection PCB) | Built-in over-charge/discharge protection |
| U1 | TC4056 USB-C charger module | Charge only; 1 A charge rate |
| SW1 | Slide/toggle switch, **rated ≥3 A** | Main ON/OFF for the system rail |
| U2 | Boost converter module, **≥2 A out**, trimpot set to **5.0 V** | e.g. 2 A boost board (not a bare MT3608) |
| C1 | Electrolytic cap **470 µF / ≥10 V** + 0.1 µF ceramic | Bulk decoupling at amp VIN |
| U3 | MAX98357A I²S amplifier breakout | GAIN→GND (12 dB) |
| U4 | INMP441 I²S MEMS mic breakout | L/R→GND (left slot) |
| LS1 | Speaker, 4 Ω or 8 Ω, rated ≥3 W | Check power rating before running loud |
| — | ESP32-S3-Touch-LCD-2.8C board | Powered from battery rail via SW1 |

---

## 2. Power path (the loop)

Energy flows: **battery → switch → splits to (ESP board) and (boost → amp)**.
Charging is independent: USB-C tops up the cell through the TC4056 even while
SW1 is OFF.

| From | Pin | To | Pin | Net | Wire |
|---|---|---|---|---|---|
| BT1 | + | U1 (TC4056) | B+ | VBATT | red, heavy |
| BT1 | − | U1 (TC4056) | B− | GND | black, heavy |
| USB-C source | — | U1 (TC4056) | USB-C jack | charge in | (cable) |
| U1 (TC4056) | OUT+ | SW1 | pin 1 | VBATT | red, heavy |
| SW1 | pin 2 | **SYS+ node** | — | VSYS (switched batt) | red, heavy |
| U1 (TC4056) | OUT− | **GND node** | — | GND | black, heavy |
| SYS+ node | — | ESP board | BAT+ / battery input | VSYS | red |
| SYS+ node | — | U2 (boost) | VIN+ | VSYS | red |
| GND node | — | U2 (boost) | VIN− | GND | black |
| U2 (boost) | VOUT+ | U3 (MAX) | Vin | **+5 V** | orange |
| U2 (boost) | VOUT− | GND node | — | GND | black |
| C1 (+) | — | U3 (MAX) Vin | — | +5 V | at the amp |
| C1 (−) | — | GND node | — | GND | at the amp |

**ESP board battery input:** wire SYS+ to whichever pin you already feed the
board from (your existing battery/VBAT input that SW1 controls). Do **not** use
the board's 5 V pin. If unsure which pin, that's the one that currently keeps
the board alive on battery.

---

## 3. Audio signal wiring (ESP J9 header → breakouts)

Two clocks (BCLK, WS) are **shared** by the amp and the mic. Everything shares
the one common ground from §2.

### MAX98357A (U3) — speaker amp

| ESP J9 pin | GPIO | → MAX98357A pin | Net | Wire |
|---|---|---|---|---|
| — | — | **Vin** ← from boost VOUT+ (§2) | +5 V | orange |
| GND | — | **GND** | GND | black |
| TXD | 43 | **BCLK** | I²S clk | blue |
| RXD | 44 | **LRC** (LRCLK/WS) | I²S clk | blue |
| D− | 19 | **DIN** | I²S data out | green |
| GND | — | **GAIN** (tie to GND = 12 dB) | config | black, dashed |
| — | — | **SD** = leave unconnected (internal pull-up enables, left ch.) | — | — |
| — | — | **+** → speaker + | spkr | brown |
| — | — | **−** → speaker − | spkr | brown |

### INMP441 (U4) — microphone

| ESP J9 pin | GPIO | → INMP441 pin | Net | Wire |
|---|---|---|---|---|
| 3V3 | — | **VDD** | 3V3 | amber |
| GND | — | **GND** | GND | black |
| TXD | 43 | **SCK** | I²S clk | blue |
| RXD | 44 | **WS** | I²S clk | blue |
| D+ | 20 | **SD** (data out) | I²S data in | purple |
| GND | — | **L/R** (tie to GND = LEFT slot) | config | black, dashed |

> The mic runs on **3V3** (do not put it on the 5 V boost). Only the amp gets 5 V.

---

## 4. Common ground (do not skip)

All of these must connect to one ground node:

- BT1 − / TC4056 B− / TC4056 OUT−
- Boost VIN− and VOUT−
- ESP board GND
- MAX98357A GND  (and C1 −)
- INMP441 GND
- INMP441 L/R, MAX98357A GAIN (both tie to this same ground)

If the amp's ground floats relative to the ESP, the I²S lines have no return
reference and the amp plays garbage or silence even with a perfect 5 V on Vin.

---

## 5. Build order (and test points)

1. **Power first, no audio yet.** Wire §2 through the boost. Set the boost
   trimpot to **5.00 V** measured at its VOUT *before* connecting it to the amp.
2. Connect the amp Vin + C1. With SW1 ON, confirm **5 V at MAX Vin to GND**, and
   with SW1 OFF confirm it drops to 0 (proves the switch gates the amp too).
3. Confirm the ESP boots normally on battery through SW1 (display + UI up).
4. **Then** wire the I²S signals in §3. Power down while soldering these.
5. Power up, play a test tone. Listen for clean output.
6. **Brownout / sag check:** play loud on battery, watch the UDP log
   (`Wireless/log_sink.c`) for `Brownout detector was triggered`. Clean log +
   clean audio = done. Distortion on peaks = boost undersized or speaker
   over-driven; whine = boost noise, add/upgrade C1.

---

## 6. Cautions

- **SW1 carries ESP + amp current.** Peaks reach ~1.5 A (4 Ω, loud). Use a
  switch rated ≥3 A or it heats, drops voltage, and re-creates the brownout.
  For a tiny switch, instead drive a P-FET high-side load switch with it and let
  the FET carry the current.
- **Battery must stay in-circuit.** It's the buffer that delivers transient
  peaks; the TC4056's 1 A cannot source them alone. Never run amp-from-USB with
  no cell installed.
- **Discharge protection trip.** The cell's protection PCB / any FS8205 trips
  around 2–3 A. Keep total peaks under that.
- **Speaker rating.** 12 dB at 5 V can push ~3 W. A 1–2 W speaker will distort
  or fail. Match the speaker to the power.
- **Boost input is the battery rail, never the 3V3 rail.** Boosting off 3V3
  overloads the onboard LDO.

---

*Companion to HARDWARE.md. Update both if the power path or audio pins change.*
