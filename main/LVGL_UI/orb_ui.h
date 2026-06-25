#pragma once

#include "lvgl.h"

typedef enum {
    ORB_BOOT = 0,
    ORB_WIFI,
    ORB_CONFIG,
    ORB_CHECK_UPD,
    ORB_UPDATING,
    ORB_WIFI_FAIL,
    ORB_SPLASH,
    ORB_LOADING,
    ORB_USER_TALK,
    ORB_AGENT,
    ORB_MUTED,
    ORB_LOW_BAT,
    ORB_ERROR,
    ORB_NFC,
} OrbState;

// Build widgets and register the 50 ms render tick. Call once after LVGL_Init().
void orb_ui_init(void);

// Thread-safe setters: any task may call. Updates are applied on the LVGL task
// during the next render tick — no mutex required.
void orb_ui_set_state(OrbState s);
void orb_ui_set_agent_name(const char *name);
void orb_ui_set_battery(float volts);
