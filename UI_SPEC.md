# Orb — UI specification

> **Purpose:** this is the source of truth for the visual design of each
> firmware state. Fill it in, hand back, I implement.
>
> **Tips:**
> - Be as specific or as vague as you want — vagueness costs iterations.
> - Reference images / mood boards / Figma URLs welcome. Drop them in
>   the `References` section at the bottom or inline.
> - Colors in `#RRGGBB`. Sizes in pixels (display is 480×480 round).
> - Times in milliseconds. Renderer is ~30 FPS (33 ms tick), so prefer
>   timings that are multiples of 33 ms for smoothness (33, 66, 100, 200,
>   330, 500, 1000…).
> - For animation easing, pick from: `linear`, `ease_in`, `ease_out`,
>   `ease_in_out`, `overshoot`, `bounce`, `step`.

---

## 0. Design system (global — applies to all states)

### Palette
| Token | Hex | Used for |
|---|---|---|
| `bg.primary` | `#160424` | Default screen background |
| `bg.secondary` | `#2b034a` | |
| `ink.high` |`#fcfcfc`| Primary text |
| `ink.muted` | `#888888` | Secondary text (name label, battery) |
| `accent.primary` | `#9716a8`| (replace or keep existing per-state colors) |
| `accent.secondary` | `#d902f5` | |
| `state.listening` | `#27AE60` (current) | USER_TALK ring/circle |
| `state.speaking` | `#1A7DC4` (current) | AGENT ring/circle |
| `state.loading` | `#C47A0C` (current) | LOADING ring/circle |
| `state.warn` | `#C0392B` (current) | LOW_BAT |
| `state.muted` | `#2A2A2A` (current) | MUTED + SPLASH dim |

### Typography
Available Montserrat sizes (precompiled): **12, 14, 16, 20, 22, 24, 26**.
Need a bigger one? Tell me — I add it via Kconfig.

| Role | Font | Size | Weight | Color |
|---|---|---|---|---|
| Agent name (top label) | Montserrat | 24 | regular | `ink.high` |
| State label (inside circle) | Montserrat | 26 | regular | `#FFFFFF` |
| Battery voltage (bottom label) | Montserrat | 20 | regular | `ink.muted` |

### Layout grid
- Display: 480 × 480, circular bezel
- Safe area: a circle of radius ~220 from center (240, 240) — content outside this risks the bezel
- Current anchors (change if you want):
  - Volume label: bot-mid, y = 400
  - Central circle: center, 200 × 200 (radius 100)
  - Battery label: bot-mid, y = 420

### Timing baseline
| Symbol | Default | Notes |
|---|---|---|
| `tick` | 33 ms | LVGL refresh period — animation frames |
| `quick` | 200 ms | Fast UI confirms (state flash) |
| `normal` | 500 ms | Standard transitions |
| `slow` | 1000 ms | Calm transitions, breathing loops |
| `breath` | 2500 ms | Whole-period for an in-and-out cycle |

---

## 1. States

> Each state below is a section. Fill what's relevant; leave the rest. If
> a state should look identical to another except for color, just say
> "same as X but color = #…".

> Order roughly matches the [`OrbState` enum](main/LVGL_UI/orb_ui.h) and
> the boot flow.

---

### `ORB_BOOT`

- **When it shows:** at power-on, before WiFi attempts. Lasts <1 s before transitioning to WIFI.
- **Lasts for:** brief, often invisible
- **Background:**
- **Central element:** Central Circle
- **Around it:**
- **Text:**
- **Animation / motion:**
- **Enter transition:**
- **Exit transition:**

---

### `ORB_WIFI`

- **When it shows:** WiFi associating. Can last 1–10 s depending on the AP.
- **Lasts for:** until got-IP
- **Background:** 
- **Central element:** Central Circle
- **Around it:**  subtle "connecting" indicator — a slowly sweeping arc
- **Text:** label says "Connecting\nWiFi"
- **Animation / motion:** the arc rotates around the central circle
- **Enter transition:**
- **Exit transition:**

---

### `ORB_CONFIG`

- **When it shows:** after got-IP, while SNTP + Supabase config GET are running.
- **Lasts for:** ~1–3 s typically
- **Background:**
- **Central element:** Central Circle
- **Around it:**  subtle "connecting" indicator — a slowly sweeping arc
- **Text:** label says "Fetching\nconfig"
- **Animation / motion:** the arc rotates around the central circle
- **Enter transition:**
- **Exit transition:**

---

### `ORB_AGENT`

- **When it shows:** agent audio is playing back through the speaker.
- **Lasts for:** length of the agent's response (typically 2–15 s)
- **Background:**
- **Central element:** Gentle pulse
- **Around it:** (suggestion: visual representation of audio output?)
- **Text:** currently "speaking"
- **Animation / motion:** (can be driven by the PCM ring buffer level or playback chunk if you want it audio-reactive)
- **Enter transition:** (from LOADING — graceful handoff?)
- **Exit transition:** (to MUTED when playback ends)

---

### `ORB_MUTED`

- **When it shows:** inside an open conversation, between turns (agent finished, waiting for user to press PTT).
- **Lasts for:** indefinite
- **Background:**
- **Central element:**
- **Around it:** Central Circle very slow breath
- **Text:** "Press\nto talk"
- **Animation / motion:**
- **Enter transition:** (from AGENT)
- **Exit transition:** (to USER_TALK when PTT pressed)

---

### `ORB_USER_TALK`

- **When it shows:** PTT held, mic streaming to agent.
- **Lasts for:** as long as button is held (typically 1–10 s)
- **Background:**
- **Central element:** Expanding central circle. Grows to 150 radius, then shrinks back to 100
- **Around it:** 
- **Text:** "Listening" 
- **Animation / motion:** (reactive to mic peak? simple pulse? both?)
- **Enter transition:** (from MUTED — should it feel sudden or graceful?)
- **Exit transition:** (to LOADING when button released)

---

### `ORB_LOADING`

- **When it shows:** two cases — (a) WSS connecting after tag scan, (b) waiting for agent response after PTT release with pad.
- **Lasts for:** typically 500 ms – 3 s
- **Background:**
- **Central element:**Central Circle
- **Around it:** sweeping arc to indicate "working"
- **Text:** currently "Loading"
- **Animation / motion:** Arc rotating around central circle
- **Enter transition:**
- **Exit transition:**

---

### `ORB_BAT` 

- **When it shows:** Battery info is always on the BOT_MID of the screen
- **Lasts for:** Always visible
- **Background:**
- **Central element:**
- **Around it:**
- **Text:** a % based on the Battery Voltage value. 3.0 volts is 0% and 4.17 is 100%. "Battery: " followed by "xxx %". The % label is green when over 65%, yellow between 30-65%, and red when under 30%.
- **Animation / motion:**
- **Enter transition:**
- **Exit transition:**

---

### `ORB_ERROR`

- **When it shows:** Cant connect to WIFI after 60s, Cant get the config, config doesnt return an agent
- **Lasts for:** until reset
- **Background:**
- **Central element:** RED Central circle 
- **Around it:** 
- **Text:** "ERROR\nRestart the Orb. "
- **Animation / motion:** breathing loop
- **Enter transition:**
- **Exit transition:** 


---

## 2. State transitions matrix

> Fill any cells you have a specific transition opinion on. Blanks mean
> "default fade / instant swap" — I'll pick something sensible.

|  From → / To ↓ | `SPLASH` | `LOADING` | `USER_TALK` | `AGENT` | `MUTED` |
|---|---|---|---|---|---|
| `SPLASH` | — | tap-to-start | — | — | — |
| `LOADING` | (failure) | — | — | first audio arrives | turn-end without audio |
| `USER_TALK` | — | PTT release | — | — | silent revert |
| `AGENT` | — | — | — | — | audio drained |
| `MUTED` | (idle timeout / session end) | PTT release after real turn | PTT pressed | new audio arrives | — |

---

## 3. Audio-reactive elements (optional, but powerful)

> If you want any visual driven by real-time audio level (mic peak when
> user is talking, playback level when agent is talking), tick yes and
> describe what should react.

- [ ] **Mic peak → visual** (during `USER_TALK`)
  - What reacts:
  - How (size? color? glow radius?):
  - Response curve: (linear / logarithmic / threshold-gated):
- [ ] **Playback level → visual** (during `AGENT`)
  - What reacts:
  - How:
  - Response curve:

---

## 4. Static images / icons (optional)

> If you want any pre-rendered graphics (logos, decorative rings, icons),
> list them here. I'll need: source file or specification, target size in
> pixels, and whether transparency is needed (ARGB8888 takes 4× the
> memory of opaque RGB565).

| Name | Where used | Size | Format | Notes |
|---|---|---|---|---|
| | | | | |

---

## 5. Sounds / haptic-like cues (audio side, but visual-adjacent)

> Currently we play a 3-tone "blopp" on NFC scan. Any other audio cues
> you want? They're cheap to add via `i2s_audio_tone_sequence`.

- NFC scan: 250 / 290 / 350 Hz × 80 / 80 / 120 ms (current, change if you want)
- Connection established (WSS opens):
- User turn start (PTT press accepted):
- User turn end (PTT release):
- Agent finished speaking:
- Error / failure (low battery, WiFi loss, config fetch failed):

---

## 6. Edge cases / error states

> What should the user see when things go wrong? Some are partially
> handled today (LOW_BAT scene exists, NFC errors are logged but not
> shown). Decide what's visible vs. silent.

- [ ] WiFi fails to connect after N attempts: ___
- [ ] Supabase config fetch fails: ___
- [ ] WSS connection fails: ___
- [ ] Agent response timeout (no audio for 15s): ___
- [ ] Mic returns zero data: ___
- [ ] PN532 not responding: ___
- [ ] PSRAM allocation failures: ___

---

## 7. References / mood board

> Drop reference images, Figma URLs, "make it feel like X" prose, anything
> that helps. I read it all.

-

---

## 8. Open questions for you to answer

> Park anything you want me to weigh in on here.

1. Do we want any persistent UI element across all states (e.g. always-visible agent name, always-visible battery, always-visible time)?
2. Is `ORB_SPLASH` the rest state visible most of the time? Should it feel alive or fully still?
3. Should state transitions feel snappy (~200 ms cuts) or graceful (~500–1000 ms fades)?
4. Any branding constraints (logo, palette tied to existing identity)?

---

## 9. Implementation priority

> Number the states 1..N in the order you want them built. Adjacent
> states share primitives — once SPLASH/MUTED/USER_TALK are done, adding
> AGENT is fast.

| Order | State | Notes |
|---|---|---|
| 1. | | |
| 2. | | |
| 3. | | |
| 4. | | |
| 5. | | |
| 6. | | |
| 7. | | |
| 8. | | |
| 9. | | |
