# Battery sensing & gauge — full reference

How the Orb measures battery level, why the on-screen number behaves the way it
does, and how the percent gauge is calibrated. Companion to the
[`BAT_Driver/`](main/BAT_Driver/) code and the battery label in
[`LVGL_UI/orb_ui.c`](main/LVGL_UI/orb_ui.c).

**Rule zero:** the ADC senses voltage **at the board, downstream of the power
switch and harness** — *not* at the battery cell. Every constant here is anchored
to that post-drop reading. Do not "fix" the gauge by trying to make the on-screen
volts equal the cell voltage; the drop is load-dependent and that chase never
converges. See [Calibration](#calibration).

---

## TL;DR

- Sensing is the **ESP32-S3 internal ADC on GPIO4** (ADC1 channel 3) via the
  2.8C's onboard divider. **No INA219, no current measurement** — voltage only.
  (The INA219 in [`BEHAVIOR.md §9`](BEHAVIOR.md) was the Raspberry-Pi build.)
- The displayed value is a **2-second moving average** (20 samples × 100 ms),
  each sample itself the mean of 16 back-to-back ADC reads.
- Percent maps **3.55 V → 0 %, 3.93 V → 100 %**, both measured *at the board,
  on battery, under load* — not at the cell.
- The orb runs in a **persistent conversation with no idle state**, so battery
  sampling cannot be gated to an idle window (this was tried and reverted).

---

## Hardware path

```
LiPo cell ──┬─────────────► TP4056 B+/B-   (USB-C here = charge port, always live)
            │
            └─ V+ ─[SWITCH]──► JST1.25 ─► board batt node ─┬─► ME6217C33 LDO ─► 3V3 ─► ESP32-S3
                                                           └─► divider ─► GPIO4 (ADC sense)
```

- **Board:** Waveshare ESP32-S3-Touch-LCD-2.8C. Onboard battery divider on
  **GPIO4**, ME6217C33M5G 3.3 V LDO, onboard charge IC (unused in our wiring).
- **Power harness (Option 1):** the LiPo is charged by an external **TP4056**
  wired permanently to the cell (its USB-C is the charge port, works with the
  orb on or off). A **switch on the cell V+** feeds the board's JST1.25
  connector. The board's *own* USB-C is **flash-only** — never used for charge,
  to avoid a two-charger conflict with the TP4056.
- **Sense point = LDO input = board battery node.** The ADC reads whatever the
  LDO sees, which is the cell voltage **minus** the IR drop across the switch +
  wiring + crimps + JST contacts. This is the single most important fact about
  the whole subsystem.

---

## The harness IR drop (why the screen reads low)

The switch + ~25 cm of wire + crimps + JST contacts form a series resistance of
roughly **0.4–0.6 Ω**. Under load that drops real voltage between the cell and
the ADC, and **the drop scales with load current** — so the reading is lowest
during heavy audio/WiFi bursts and highest when the conversation is quiet.

Bench measurements (multimeter at cell vs. on-screen, full-ish charge):

| Condition | Cell | Screen (board) | Drop |
|---|---|---|---|
| Heavy load (audio + WiFi burst) | 3.96 V | 3.77 V | ~190 mV |
| Light load | 4.12 V | 4.02 V | ~100 mV |
| Full charge, light operating load | 4.05 V | 4.03 V | ~20 mV |
| Cell sag alone (no-load → load) | 4.09 → 4.05 V | — | ~40 mV |

Takeaways:
- The **cell is healthy** — only ~40 mV of its own sag under load. The harness,
  not the battery, is the dominant loss.
- The drop ranges **~20–190 mV with instantaneous load**, which is why a fixed
  voltage offset can't make the screen match the cell. The 2-second average
  smooths it; it does not remove it.
- **The only real fix is lower harness resistance** — a low-Rds switch (or P-FET
  load switch), solid crimps, shorter/thicker V+ wire. That reclaims the bottom
  ~15–20 % of runtime *and* tightens the gauge.

### Charging note
The cell rests at ~4.09 V after charge (a healthy LiPo rests ~4.15–4.20 V),
i.e. a slightly **soft full**. Most likely the TP4056 terminated early because
the orb was drawing load while charging (load-sharing confuses charge
termination). **Charge with the orb switched off** for a fuller top-off.

---

## Firmware data path

1. [`BAT_Driver.c::BAT_Get_Volts()`](main/BAT_Driver/BAT_Driver.c) — reads the
   ADC, applies IDF calibration + `Measurement_offset`, and returns the
   **moving-average** voltage in `BAT_analogVolts`.
2. [`main.c::Driver_Loop`](main/main.c) — calls `BAT_Get_Volts()` then
   `orb_ui_set_battery()` every **100 ms**.
3. [`orb_ui.c::orb_ui_set_battery()`](main/LVGL_UI/orb_ui.c) — stores volts as
   millivolts in an atomic; the 50 ms LVGL render tick maps it to percent +
   colour and renders the label.

### Averaging (two stacked stages)
- **16× burst per read** — `BAT_SAMPLES = 16` back-to-back `adc_oneshot_read`s,
  meaned. Kills per-sample ADC noise.
- **20-sample sliding window** — `BAT_AVG_WINDOW = 20` burst-means in a ring
  buffer; the windowed mean is reported. At the 100 ms loop rate that is a
  **~2.0 s window** (≈320 raw reads behind the number), lagging a real change by
  ~1 s and settling in ~2 s. The window fills gradually on boot.
- **Tuning knob:** change `BAT_AVG_WINDOW` only. 30 → ~3 s (calmer), 10 → ~1 s
  (snappier).

### Label format
`"%d%% - %d.%02dV"` → e.g. `73% - 3.90V`. The voltage is formatted from integer
millivolts (`mv/1000`, `(mv%1000)/10`) **on purpose**: LVGL's `lv_label_set_text_fmt`
has `%f` disabled in the locked LV config, so `%.2f` renders as a stray "fV". Do
not switch back to a float format. (See the LVGL lock note in
[CLAUDE.md](CLAUDE.md) / [SCREEN.md](SCREEN.md).)

---

## Calibration

Constants live at the top of [`orb_ui.c`](main/LVGL_UI/orb_ui.c):

```c
#define BATTERY_MV_EMPTY    3550   // 0 %  — ~LDO brown-out point at the board
#define BATTERY_MV_FULL     3930   // 100% — on-screen volts at a full cell, unplugged, under load
```

`pct = (mv − EMPTY) * 100 / (FULL − EMPTY)`, clamped 0–100. Colour: green > 65 %,
amber 30–65 %, red < 30 %.

Reference points on the current 3.55–3.93 V scale:

| Screen | % |
|---|---|
| 3.93 V | 100 |
| 3.74 V | ~50 |
| 3.55 V | 0 |

**Why these values (and why not cell voltage):**
- `FULL = 3930` is the **on-screen** reading at a full cell **on battery
  (unplugged) under load** (3.94 V measured). Anchoring to the unplugged
  under-load reading makes a full pack read 100 % in *every* condition: charging
  reads ~4.01 V and clamps, light load reads higher and clamps. **Tradeoff:**
  because it's anchored to a heavier-load (lower) reading, the gauge sticks at
  100 % through the top ~15–20 % of discharge before it starts to move — a touch
  optimistic early, accurate later. The alternative (anchor ~4.00) is honest
  earlier but dips below 100 % under load at full charge; we chose the
  "full = 100 %" behaviour deliberately.
- `EMPTY = 3550` targets the **LDO brown-out edge** at the board (the ME6217
  needs ~3.5 V in to hold 3.3 V out), so 0 % lands just before the orb resets
  instead of it dying at ~27 %. This is a **placeholder anchored to an estimate** —
  refine it with a real brown-out reading (see Open items).
- Both anchors are **board-after-drop** readings, matching where the ADC sits.
  Anchoring to cell voltage would make the gauge read low at the top and die at a
  high percent.

The gauge is **purely cosmetic** — nothing in firmware cuts power at these
thresholds. Real over-discharge protection is the TP4056/DW01 cutting the cell
off at ~2.5 V. (The lone `ORB_LOW_BAT` use in [`nfc.c`](main/NFC/nfc.c) is a
misused error state for "no agent_id", not a battery check.)

---

## Why there is no idle-gated sampling

[`BEHAVIOR.md §9.2`](BEHAVIOR.md) specifies sampling only while idle
(`splash_idle`) to dodge load-sag. **That does not apply to this firmware.**
[`Wireless.c::post_connect_task`](main/Wireless/Wireless.c) boots the orb
straight `CONFIG → LOADING → convai_start` into a persistent conversation;
`ORB_SPLASH` is defined but never set at runtime. An attempt to gate sampling to
`ORB_SPLASH` left the label permanently blank and was reverted. If a true idle
state is ever reintroduced, idle-gating becomes worthwhile again.

---

## Open items

- **Anchor `EMPTY` to a real brown-out reading.** Run the pack down and note the
  on-screen volts the instant the orb resets; set `BATTERY_MV_EMPTY` to that.
- **Lower the harness resistance** (switch/crimps/wire). The biggest single
  improvement: reclaims bottom-end runtime and tightens the load-dependent swing.
- **Charge with the orb off** to confirm the cell tops to ~4.15 V rested; if so,
  the soft-full goes away.
- Current-sense (charge/discharge mA) is **not available** without an INA219 —
  out of scope for the GPIO4 divider.
