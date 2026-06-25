#include "LVGL_Driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdatomic.h>

// Incremented every iteration of lvgl_task. Heartbeat consumers compare
// successive reads to detect a stalled LVGL task (delta of 0 over a
// heartbeat window = the screen is frozen).
static _Atomic uint32_t s_lvgl_iter = 0;
uint32_t LVGL_GetIterCounter(void) { return atomic_load(&s_lvgl_iter); }

// Direct port of github.com/traviscea/right-side-cluster-esp32s3 LVGL 8
// driver. Touch (GT911) removed; otherwise byte-identical to the
// confirmed-working reference. Do not rearchitect.

static const char *LVGL_TAG = "LVGL";
lv_disp_draw_buf_t disp_buf;
lv_disp_drv_t disp_drv;
lv_disp_t *disp = NULL;

esp_timer_handle_t lvgl_tick_timer = NULL;

static void *buf1 = NULL;
static void *buf2 = NULL;
#define DRAW_BUF_LINES 60

static SemaphoreHandle_t s_lvgl_mutex = NULL;

void example_lvgl_flush_cb(lv_disp_drv_t *drv,
                           const lv_area_t *area,
                           lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    esp_lcd_panel_draw_bitmap(panel,
                              area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1,
                              color_map);
    lv_disp_flush_ready(drv);
}

void example_increase_lvgl_tick(void *arg)
{
    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(1);
}

static void lvgl_task(void *arg)
{
    while (1) {
        uint32_t delay_ms;
        xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
        delay_ms = lv_timer_handler();
        xSemaphoreGive(s_lvgl_mutex);
        atomic_fetch_add(&s_lvgl_iter, 1);
        if (delay_ms == 0) delay_ms = 1;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

bool LVGL_Lock(uint32_t timeout_ms)
{
    if (!s_lvgl_mutex) return false;
    TickType_t wait = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(s_lvgl_mutex, wait) == pdTRUE;
}

void LVGL_Unlock(void)
{
    if (s_lvgl_mutex) xSemaphoreGive(s_lvgl_mutex);
}

void LVGL_Init(void)
{
    ESP_LOGI(LVGL_TAG, "Initialize LVGL library");
    lv_init();

    s_lvgl_mutex = xSemaphoreCreateMutex();
    assert(s_lvgl_mutex);

#if CONFIG_EXAMPLE_DOUBLE_FB
    ESP_LOGI(LVGL_TAG, "Use frame buffers as LVGL draw buffers");
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 2, &buf1, &buf2));
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2,
                          EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES);
#else
    buf1 = heap_caps_malloc(EXAMPLE_LCD_H_RES * DRAW_BUF_LINES * sizeof(lv_color_t),
                            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    buf2 = heap_caps_malloc(EXAMPLE_LCD_H_RES * DRAW_BUF_LINES * sizeof(lv_color_t),
                            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2,
                          EXAMPLE_LCD_H_RES * DRAW_BUF_LINES);
#endif

    ESP_LOGI(LVGL_TAG, "Register display driver to LVGL");
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = EXAMPLE_LCD_H_RES;
    disp_drv.ver_res = EXAMPLE_LCD_V_RES;
    disp_drv.flush_cb = example_lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;
    disp = lv_disp_drv_register(&disp_drv);

    ESP_LOGI(LVGL_TAG, "Install LVGL tick timer");
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, 1000));   // 1 ms

    xTaskCreatePinnedToCore(lvgl_task, "lvgl_task", 8192, NULL, 5, NULL, 1);
}
