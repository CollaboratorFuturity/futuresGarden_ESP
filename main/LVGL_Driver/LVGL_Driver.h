#pragma once
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "ST7701S.h"

#define EXAMPLE_LVGL_TICK_PERIOD_MS    5

extern lv_disp_draw_buf_t disp_buf;
extern lv_disp_drv_t disp_drv;
extern lv_disp_t *disp;

void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map);
void example_increase_lvgl_tick(void *arg);

void LVGL_Init(void);

// Mutex wrapped around the LVGL task so cross-task widget mutations are
// safe. timeout_ms=0 blocks forever.
#include <stdbool.h>
#include <stdint.h>
bool LVGL_Lock(uint32_t timeout_ms);
void LVGL_Unlock(void);

// Monotonically increasing counter incremented once per lvgl_task iteration.
// Heartbeat uses delta-over-window to detect a frozen LVGL task.
uint32_t LVGL_GetIterCounter(void);
