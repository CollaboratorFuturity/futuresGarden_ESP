#include "orb_ui.h"

#include <stdatomic.h>
#include <string.h>
#include <stdio.h>

#include "nfc.h"

#define ORB_CIRCLE_DIAM     200
#define ORB_NAME_Y          90        // (legacy — name now lives inside the circle)
#define ORB_BATTERY_Y       420       // UI_SPEC.md §0: bot-mid, y = 420
#define ORB_TICK_MS         50
#define ORB_NAME_MAX        32

// Battery → percent mapping. Anchored to ADC-at-the-board readings (after the
// switch + harness IR drop), NOT raw cell voltage, since the ADC senses
// downstream of the drop. FULL = on-screen volts at a full cell, on battery
// (unplugged) under load (3.94 V measured) — anchored to the unplugged
// under-load reading so a full pack reads 100 % in every condition (charging
// reads ~4.01 V and just clamps). Tradeoff: the gauge sticks at 100 % through
// the top ~15-20 % of discharge before it starts moving. EMPTY = ~the LDO
// brown-out point at the board (~3.5 V in to hold 3.3 V out), so 0 % lands just
// before the orb resets. Refine EMPTY once a real brown-out reading is captured.
#define BATTERY_MV_EMPTY    3550
#define BATTERY_MV_FULL     3930
// Threshold colours for the percent label.
#define BATTERY_COLOR_OK    0x27AE60  // green   — > 65 %
#define BATTERY_COLOR_MID   0xC47A0C  // amber   — 30..65 %
#define BATTERY_COLOR_LOW   0xC0392B  // red     — < 30 %

typedef struct {
    uint32_t color;
    const char *label;
} OrbStateStyle;

// State styles per UI_SPEC.md §1. Colors map to the design-system tokens in
// UI_SPEC.md §0 (state.listening / state.speaking / state.loading /
// state.warn / state.muted). BOOT/WIFI/CONFIG use a neutral dark grey since
// the spec leaves their circle color unspecified.
static const OrbStateStyle STATE_STYLE[] = {
    [ORB_BOOT]      = { 0x404040, "Booting" },
    [ORB_WIFI]      = { 0x404040, "Connecting\nWiFi" },
    [ORB_CONFIG]    = { 0x404040, "Fetching\nconfig" },
    [ORB_CHECK_UPD] = { 0x404040, "Checking\nfor updates" },
    [ORB_UPDATING]  = { 0x404040, "Updating" },
    [ORB_WIFI_FAIL] = { 0xC0392B, "No WiFi. Check\nrouter & restart" },
    [ORB_SPLASH]    = { 0x2A2A2A, "Ready" },
    [ORB_LOADING]   = { 0xC47A0C, "Loading\n(v1)" },
    [ORB_USER_TALK] = { 0x27AE60, "Listening" },
    [ORB_AGENT]     = { 0x1A7DC4, "Agent\nspeaking" },
    [ORB_MUTED]     = { 0x2A2A2A, "Press\nto talk" },
    [ORB_LOW_BAT]   = { 0xC0392B, "Low battery" },
    [ORB_ERROR]     = { 0xC0392B, "ERROR\nRestart the Orb." },
    [ORB_NFC]       = { 0x7B2FA8, "NFC\nScanned" },
};

static lv_obj_t *s_circle;
static lv_obj_t *s_circle_label;
static lv_obj_t *s_name_label;
static lv_obj_t *s_battery_label;

// "Connecting" indicator (UI_SPEC.md §1 — WIFI, CONFIG, LOADING):
// a slowly sweeping arc that rotates around the central circle.
// Implemented as a single lv_arc with a fixed angular span; we animate
// lv_arc_set_rotation in a continuous 0..360 loop. LVGL only invalidates
// the arc's bounding box per frame — cheap enough at 15 ms LVGL refresh.
#define ARC_SIZE             260          // outer diameter, sits just outside the 200 px central circle
#define ARC_WIDTH            8            // stroke thickness
#define ARC_SPAN_DEG         70           // visible arc length
#define ARC_PERIOD_MS        1800         // one full revolution
#define ARC_COLOR            0x9716A8     // accent.primary
static lv_obj_t *s_sweep_arc = NULL;
static lv_anim_t s_sweep_anim;

// Central-circle pulse / breath (UI_SPEC.md §1 — USER_TALK, AGENT, MUTED,
// ERROR). One shared animation, one preset per state. Each preset declares
// the diameter range, half-period (the second half is the reverse playback),
// and easing curve. Setting half_period_ms = 0 means "no animation, just
// hold at min_diam" — used for the states that don't pulse.
typedef struct {
    int     min_diam;
    int     max_diam;
    uint32_t half_period_ms;
    lv_anim_path_cb_t path;
} CirclePulsePreset;

static const CirclePulsePreset PULSE_NONE  = { ORB_CIRCLE_DIAM, ORB_CIRCLE_DIAM,    0, NULL };
static const CirclePulsePreset PULSE_TALK  = { 200, 300,  350, lv_anim_path_ease_in_out };  // 700 ms cycle, active
static const CirclePulsePreset PULSE_AGENT = { 200, 220,  750, lv_anim_path_ease_in_out };  // 1500 ms, gentle
static const CirclePulsePreset PULSE_MUTED = { 200, 212, 1250, lv_anim_path_ease_in_out };  // 2500 ms breath
static const CirclePulsePreset PULSE_ERROR = { 200, 212, 1250, lv_anim_path_ease_in_out };  // same shape, red

static lv_anim_t s_circle_anim;

// State transitions cross-fade through opacity 0 (UI_SPEC.md §0 — `quick`).
// 200 ms out + 200 ms in = 400 ms total transition. The actual state swap
// (color, text, animation restart) happens at the 0-opacity midpoint so the
// user never sees half-rendered content.
#define FADE_DURATION_MS  200
static volatile bool s_in_transition  = false;
static OrbState      s_transition_to  = ORB_BOOT;

// Shadow state written by background tasks, applied on the LVGL task.
// atomics avoid tearing on the OrbState/float reads; the char buffer is guarded
// by a sequence flag so the consumer always sees a complete string.
static _Atomic OrbState s_pending_state = ATOMIC_VAR_INIT(ORB_BOOT);
static _Atomic OrbState s_applied_state = ATOMIC_VAR_INIT((OrbState)-1);

static char s_pending_name[ORB_NAME_MAX];
static char s_applied_name[ORB_NAME_MAX];
static _Atomic bool s_name_dirty = ATOMIC_VAR_INIT(false);

static _Atomic int s_pending_battery_mv = ATOMIC_VAR_INIT(-1);  // millivolts, -1 = unset
static int s_applied_battery_mv = -2;

// LVGL animation callback — updates the arc's rotation each frame.
static void sweep_arc_rotate_cb(void *var, int32_t val)
{
    lv_arc_set_rotation((lv_obj_t *)var, val);
}

// LVGL animation callback — updates the central circle's diameter. LVGL
// keeps it centered automatically because s_circle's persistent alignment
// is LV_ALIGN_CENTER (set in orb_ui_init).
static void circle_size_cb(void *var, int32_t diam)
{
    lv_obj_set_size((lv_obj_t *)var, diam, diam);
}

static void apply_circle_pulse(const CirclePulsePreset *p)
{
    lv_anim_del(s_circle, circle_size_cb);
    if (p->half_period_ms == 0) {
        // No animation — restore the resting size.
        lv_obj_set_size(s_circle, p->min_diam, p->min_diam);
        return;
    }
    lv_anim_init(&s_circle_anim);
    lv_anim_set_var(&s_circle_anim, s_circle);
    lv_anim_set_values(&s_circle_anim, p->min_diam, p->max_diam);
    lv_anim_set_time(&s_circle_anim, p->half_period_ms);
    lv_anim_set_playback_time(&s_circle_anim, p->half_period_ms);
    lv_anim_set_repeat_count(&s_circle_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&s_circle_anim, circle_size_cb);
    lv_anim_set_path_cb(&s_circle_anim, p->path);
    lv_anim_start(&s_circle_anim);
}

static void apply_state(OrbState s)
{
    const OrbStateStyle *st = &STATE_STYLE[s];
    lv_color_t c = lv_color_hex(st->color);
    lv_obj_set_style_bg_color(s_circle, c, LV_PART_MAIN);
    lv_label_set_text(s_circle_label, st->label);

    // Sweeping arc lives outside the central circle during the three
    // "working" states. Hidden + animation deleted in every other state so
    // there's zero animation cost when idle.
    bool show_sweep = (s == ORB_WIFI || s == ORB_CONFIG || s == ORB_LOADING ||
                       s == ORB_UPDATING || s == ORB_CHECK_UPD);
    if (s_sweep_arc) {
        if (show_sweep) {
            lv_obj_clear_flag(s_sweep_arc, LV_OBJ_FLAG_HIDDEN);
            lv_anim_start(&s_sweep_anim);
        } else {
            lv_anim_del(s_sweep_arc, sweep_arc_rotate_cb);
            lv_obj_add_flag(s_sweep_arc, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Central-circle pulse / breath — preset per state, all four motion
    // states share the same animation machinery.
    const CirclePulsePreset *pulse = &PULSE_NONE;
    switch (s) {
        case ORB_USER_TALK: pulse = &PULSE_TALK;  break;
        case ORB_AGENT:     pulse = &PULSE_AGENT; break;
        case ORB_MUTED:     pulse = &PULSE_MUTED; break;
        case ORB_ERROR:     pulse = &PULSE_ERROR; break;
        default: break;
    }
    apply_circle_pulse(pulse);
}

// Sets opacity on every object that participates in the state-change fade
// (everything that's part of the central composition). Battery label is
// intentionally excluded so it stays visible across transitions.
static void fade_opa_apply(lv_opa_t opa)
{
    lv_obj_set_style_opa(s_circle,        opa, LV_PART_MAIN);
    lv_obj_set_style_opa(s_circle_label,  opa, LV_PART_MAIN);
    lv_obj_set_style_opa(s_name_label,    opa, LV_PART_MAIN);
    if (s_sweep_arc) {
        lv_obj_set_style_arc_opa(s_sweep_arc, opa, LV_PART_INDICATOR);
    }
}

static void fade_opa_anim_cb(void *var, int32_t v)
{
    (void)var;
    fade_opa_apply((lv_opa_t)v);
}

static void fade_in_ready_cb(lv_anim_t *a)
{
    (void)a;
    s_in_transition = false;
}

static void fade_out_ready_cb(lv_anim_t *a)
{
    (void)a;
    // Midpoint: swap to the new state (color, text, restart sub-animations
    // like sweep arc and circle pulse) while the content is invisible.
    apply_state(s_transition_to);
    atomic_store(&s_applied_state, s_transition_to);

    // Now fade back in.
    static lv_anim_t fade_in;
    lv_anim_init(&fade_in);
    lv_anim_set_var(&fade_in, s_circle);
    lv_anim_set_values(&fade_in, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&fade_in, FADE_DURATION_MS);
    lv_anim_set_exec_cb(&fade_in, fade_opa_anim_cb);
    lv_anim_set_path_cb(&fade_in, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&fade_in, fade_in_ready_cb);
    lv_anim_start(&fade_in);
}

static void begin_transition(OrbState target)
{
    s_in_transition = true;
    s_transition_to = target;

    // Cancel any prior fade animation so a rapid state change doesn't
    // collide with a half-finished previous transition.
    lv_anim_del(s_circle, fade_opa_anim_cb);

    static lv_anim_t fade_out;
    lv_anim_init(&fade_out);
    lv_anim_set_var(&fade_out, s_circle);
    lv_anim_set_values(&fade_out, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&fade_out, FADE_DURATION_MS);
    lv_anim_set_exec_cb(&fade_out, fade_opa_anim_cb);
    lv_anim_set_path_cb(&fade_out, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&fade_out, fade_out_ready_cb);
    lv_anim_start(&fade_out);
}

static void orb_tick_cb(lv_timer_t *t)
{
    OrbState pending = atomic_load(&s_pending_state);
    OrbState applied = atomic_load(&s_applied_state);
    if (pending != applied && !s_in_transition) {
        if (applied == (OrbState)-1) {
            // First apply at boot — content is already rendered correctly
            // (orb_ui_init wrote BOOT styling); just commit without fading.
            apply_state(pending);
            atomic_store(&s_applied_state, pending);
        } else {
            begin_transition(pending);
        }
    }

    if (atomic_exchange(&s_name_dirty, false)) {
        // pending_name may be mid-write from another task; copy under the
        // assumption writers are infrequent and short. Worst case: a torn
        // string flashes for one tick before the next write settles it.
        strncpy(s_applied_name, s_pending_name, ORB_NAME_MAX - 1);
        s_applied_name[ORB_NAME_MAX - 1] = '\0';
        lv_label_set_text(s_name_label, s_applied_name);
    }

    int mv = atomic_load(&s_pending_battery_mv);
    if (mv != s_applied_battery_mv) {
        s_applied_battery_mv = mv;
        if (mv < 0) {
            lv_label_set_text(s_battery_label, "");
        } else {
            // Percent from BATTERY_MV_EMPTY (3.55 V → 0 %) to
            // BATTERY_MV_FULL (3.93 V → 100 %), clamped. Anchors are
            // board-after-harness-drop readings, not cell voltage — see BATTERY.md.
            int span = BATTERY_MV_FULL - BATTERY_MV_EMPTY;
            int pct  = ((mv - BATTERY_MV_EMPTY) * 100) / span;
            if (pct < 0)   pct = 0;
            if (pct > 100) pct = 100;

            uint32_t colour;
            if      (pct > 65) colour = BATTERY_COLOR_OK;
            else if (pct >= 30) colour = BATTERY_COLOR_MID;
            else               colour = BATTERY_COLOR_LOW;

            // LVGL's lv_label_set_text_fmt has float (%f) support disabled in
            // the locked LV config, so format the voltage from integer mv:
            // 3940 mv -> "3.94V".
            int v_whole = mv / 1000;
            int v_frac  = (mv % 1000) / 10;   // hundredths of a volt
            lv_obj_set_style_text_color(s_battery_label, lv_color_hex(colour), 0);
            lv_label_set_text_fmt(s_battery_label, "%d%% - %d.%02dV", pct, v_whole, v_frac);
        }
    }
}

void orb_ui_init(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x160424), LV_PART_MAIN);   // bg.primary
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Centre circle
    s_circle = lv_obj_create(scr);
    lv_obj_remove_style_all(s_circle);
    lv_obj_set_size(s_circle, ORB_CIRCLE_DIAM, ORB_CIRCLE_DIAM);
    lv_obj_set_style_radius(s_circle, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_circle, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_circle, lv_color_hex(STATE_STYLE[ORB_BOOT].color), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_circle, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_circle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_circle, LV_ALIGN_CENTER, 0, 0);

    // Sweeping arc — rotates around the central circle during WIFI / CONFIG
    // / LOADING (UI_SPEC.md §1). Uses lv_arc with a fixed angular span; the
    // animation rotates the whole widget by updating lv_arc_set_rotation.
    s_sweep_arc = lv_arc_create(scr);
    lv_obj_remove_style_all(s_sweep_arc);
    lv_obj_set_size(s_sweep_arc, ARC_SIZE, ARC_SIZE);
    lv_obj_align(s_sweep_arc, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(s_sweep_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style(s_sweep_arc, NULL, LV_PART_KNOB);
    // MAIN part = the full background ring — keep invisible so we only see
    // the sweeping indicator, not a static full ring underneath.
    lv_obj_set_style_arc_opa(s_sweep_arc, LV_OPA_TRANSP, LV_PART_MAIN);
    // INDICATOR part = the actual sweeping arc.
    lv_obj_set_style_arc_color(s_sweep_arc, lv_color_hex(ARC_COLOR), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_sweep_arc, ARC_WIDTH, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_sweep_arc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_arc_set_bg_angles(s_sweep_arc, 0, 360);
    lv_arc_set_angles(s_sweep_arc, 0, ARC_SPAN_DEG);
    lv_arc_set_rotation(s_sweep_arc, 0);
    lv_obj_add_flag(s_sweep_arc, LV_OBJ_FLAG_HIDDEN);   // apply_state shows it

    // Pre-built animation. 360→0 wraparound is invisible (same screen angle),
    // so a plain repeating 0..360 ramp gives a seamless continuous rotation.
    lv_anim_init(&s_sweep_anim);
    lv_anim_set_var(&s_sweep_anim, s_sweep_arc);
    lv_anim_set_values(&s_sweep_anim, 0, 360);
    lv_anim_set_time(&s_sweep_anim, ARC_PERIOD_MS);
    lv_anim_set_repeat_count(&s_sweep_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&s_sweep_anim, sweep_arc_rotate_cb);
    lv_anim_set_path_cb(&s_sweep_anim, lv_anim_path_linear);

    // Agent name label — inside the circle, above the state label.
    // Typography per UI_SPEC.md §0: Montserrat 24, ink.muted (#888888).
    s_name_label = lv_label_create(s_circle);
    lv_label_set_text(s_name_label, "----");
    lv_obj_set_style_text_color(s_name_label, lv_color_hex(0x888888), 0);
#if LV_FONT_MONTSERRAT_24
    lv_obj_set_style_text_font(s_name_label, &lv_font_montserrat_24, 0);
#elif LV_FONT_MONTSERRAT_20
    lv_obj_set_style_text_font(s_name_label, &lv_font_montserrat_20, 0);
#endif
    lv_obj_align(s_name_label, LV_ALIGN_CENTER, 0, -22);

    // State label — Montserrat 26 / white, centred below the agent name.
    // Some states use multi-line text (e.g. "Connecting\nWiFi"); centre-
    // align so both lines stack symmetrically.
    s_circle_label = lv_label_create(s_circle);
    lv_label_set_text(s_circle_label, STATE_STYLE[ORB_BOOT].label);
    lv_obj_set_style_text_color(s_circle_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(s_circle_label, LV_TEXT_ALIGN_CENTER, 0);
#if LV_FONT_MONTSERRAT_26
    lv_obj_set_style_text_font(s_circle_label, &lv_font_montserrat_26, 0);
#elif LV_FONT_MONTSERRAT_22
    lv_obj_set_style_text_font(s_circle_label, &lv_font_montserrat_22, 0);
#endif
    lv_obj_align(s_circle_label, LV_ALIGN_CENTER, 0, 18);

    // Battery label — Montserrat 20 / colour by level. Format "Battery: NN %".
    s_battery_label = lv_label_create(scr);
    lv_label_set_text(s_battery_label, "");
    lv_obj_set_style_text_color(s_battery_label, lv_color_hex(0x888888), 0);
#if LV_FONT_MONTSERRAT_20
    lv_obj_set_style_text_font(s_battery_label, &lv_font_montserrat_20, 0);
#endif
    lv_obj_align(s_battery_label, LV_ALIGN_TOP_MID, 0, ORB_BATTERY_Y);

    lv_timer_create(orb_tick_cb, ORB_TICK_MS, NULL);
}

void orb_ui_set_state(OrbState s)
{
    atomic_store(&s_pending_state, s);

    // Gate NFC scanning on the turn state. Tags may only be read in the idle
    // ORB_MUTED window (between turns, before the user presses PTT). Scanning
    // is suppressed while the user is talking (ORB_USER_TALK), while we wait
    // for the agent (ORB_LOADING), and while the agent is speaking (ORB_AGENT),
    // so a tag read can't interrupt or corrupt an active turn. orb_ui_set_state
    // is the single point every module routes turn transitions through, which
    // makes it the natural choke point for this. (NFC_Set_Polling just flips a
    // volatile flag — safe from any task/context.)
    NFC_Set_Polling(s == ORB_MUTED);
}

void orb_ui_set_agent_name(const char *name)
{
    if (!name) name = "";
    strncpy(s_pending_name, name, ORB_NAME_MAX - 1);
    s_pending_name[ORB_NAME_MAX - 1] = '\0';
    atomic_store(&s_name_dirty, true);
}

void orb_ui_set_battery(float volts)
{
    int mv = (int)(volts * 1000.0f);
    if (mv < 0) mv = 0;
    atomic_store(&s_pending_battery_mv, mv);
}
