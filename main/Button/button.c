#include "button.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#define BUTTON_GPIO     GPIO_NUM_0
#define DEBOUNCE_MS     50    // BEHAVIOR.md §3.1 PTT debounce constant

static const char *TAG = "button";
static QueueHandle_t s_evt_q;
static button_state_cb_t s_cb;

// ISR fires on ANY edge (negedge OR posedge). We just nudge the queue —
// the task confirms the actual level after debouncing.
static void IRAM_ATTR button_isr(void *arg)
{
    uint32_t tick = xTaskGetTickCountFromISR();
    BaseType_t hpw = pdFALSE;
    xQueueSendFromISR(s_evt_q, &tick, &hpw);
    if (hpw) portYIELD_FROM_ISR();
}

static void button_task(void *arg)
{
    bool last_state = false;   // false = released (line HIGH), true = pressed (line LOW)
    TickType_t last_change_tick = 0;

    while (1) {
        uint32_t edge_tick;
        if (xQueueReceive(s_evt_q, &edge_tick, portMAX_DELAY) != pdTRUE) continue;

        // Reject any edge within DEBOUNCE_MS of the last accepted change.
        if ((TickType_t)edge_tick - last_change_tick < pdMS_TO_TICKS(DEBOUNCE_MS)) continue;

        // Settle, then re-read the line to confirm the new stable state.
        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
        bool new_state = (gpio_get_level(BUTTON_GPIO) == 0);  // LOW = pressed

        if (new_state == last_state) continue;  // bounced back

        last_state = new_state;
        last_change_tick = xTaskGetTickCount();
        if (s_cb) s_cb(new_state);
    }
}

void button_init(button_state_cb_t cb)
{
    s_cb = cb;

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        // Both edges so we catch press AND release.
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    s_evt_q = xQueueCreate(8, sizeof(uint32_t));
    // Callbacks may do heavy work (audio, UDP logging) — give the task real
    // headroom. See the stack-overflow incident on the original 2.5 KB.
    xTaskCreatePinnedToCore(button_task, "button", 6144, NULL, 5, NULL, 0);

    // Idempotent; safe if another component has already installed the ISR
    // service.
    gpio_install_isr_service(0);
    ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON_GPIO, button_isr, NULL));

    ESP_LOGI(TAG, "PTT button ready on GPIO %d (debounce %d ms, edge=ANY)",
             BUTTON_GPIO, DEBOUNCE_MS);
}
