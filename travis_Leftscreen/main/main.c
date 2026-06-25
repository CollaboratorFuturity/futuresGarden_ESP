#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "TCA9554PWR.h"
#include "ST7701S.h"
#include "GT911.h"
#include "LVGL_Driver.h"
#include "boot/boot_screen.h"
#include "ui/screens/ui_Screen1.h"
#include "ui/ui.h"
#include "driver/uart.h"
#include "esp_log.h"
#include <string.h>
#include <stdint.h>
#include <math.h>

static lv_color_t green_color;
static lv_color_t red_color;
static lv_color_t orange_color;
static lv_color_t purple_color;
static lv_color_t pink_color;
static lv_color_t blue_color;

static bool low_fuel_blink_state = false;

#define UART_PORT      UART_NUM_1
#define UART_RX_PIN    44
#define UART_BAUD      2000000

#define ENABLE_LOGGING 0

#define SOF_BYTE       0xA5
#define PKT_LEN        26

static const char *TAG = "RX";

typedef struct __attribute__((packed)) {
    uint16_t oil_temp;      
    uint16_t water_temp;    
    uint16_t oil_pressure;  
    uint16_t fuel_pressure; 
    uint16_t fuel_level;   
    uint16_t afr;         
    int16_t boost;          
    uint32_t lap_time_ms;    
    int32_t  lap_delta_ms; 
} gauge_payload_t;

static const float BASE_FUEL_PRESSURE = 43.0;

static int g_water_temp = 70;
static int g_oil_temp = 70;
static int g_oil_pressure = 70;
static int g_fuel_pressure = 70;
static int g_fuel_level = 50;
static int g_boost = 0;
static const bool g_use_fake_sensor_data = true;

static uint16_t crc16_ccitt(const uint8_t *data, uint16_t len){
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
    }
    return crc;
}




static void uart_init(void)
{
    uart_config_t cfg = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT
    };

    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));

    ESP_ERROR_CHECK(uart_set_pin(
        UART_PORT,
        UART_PIN_NO_CHANGE,
        UART_RX_PIN,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE
    ));

    ESP_ERROR_CHECK(uart_driver_install(
        UART_PORT,
        4096,   // RX buffer (bigger)
        0,
        0,
        NULL,
        0
    ));
}


void update_gauge_label(lv_obj_t *label, float value)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%.2f", value);
    lv_label_set_text(label, buf);
}

static void uart_rx_task(void *arg){
    uint8_t buf[PKT_LEN];
    int idx = 0;

    while (1) {
        uint8_t byte;
        if (uart_read_bytes(UART_PORT, &byte, 1, portMAX_DELAY) != 1)
            continue;

        // sync on SOF
        if (idx == 0 && byte != SOF_BYTE)
            continue;

        buf[idx++] = byte;

        if (idx == PKT_LEN) {
            idx = 0;

            uint16_t rx_crc = buf[PKT_LEN - 2] | (buf[PKT_LEN - 1] << 8);

            uint16_t calc_crc = crc16_ccitt(&buf[1], PKT_LEN - 3);

            if (rx_crc != calc_crc) {
                ESP_LOGW(TAG, "CRC fail");
                continue;
            }

            gauge_payload_t p;
            memcpy(&p, &buf[2], sizeof(p));

            float afr = p.afr * 0.1f; 
            float boost = p.boost * 0.1f;
            float water_temp = p.water_temp *0.1f;
            float oil_temp = p.oil_temp *0.1f;
            float oil_pressure = p.oil_pressure *0.1f;
            float fuel_pressure = p.fuel_pressure *0.1f;
            float fuel_level = p.fuel_level *0.1f;

            g_water_temp = (int)water_temp;
            g_oil_temp = (int)oil_temp;
            g_oil_pressure = (int)oil_pressure;
            g_fuel_pressure = (int)fuel_pressure;
            g_fuel_level = (int)fuel_level;
            g_boost = boost;

            #if ENABLE_LOGGING
            ESP_LOGI(TAG,
                "Oil %.1fC Water %.1fC AFR %.2f Boost %.1fkPa",
                p->oil_temp / 10.0f,
                p->water_temp / 10.0f,
                p->afr / 10.0f,
                p->boost / 10.0f
            );
            #endif
        }
    }
}




static inline int map_int(int x,
                          int in_min, int in_max,
                          int out_min, int out_max)
{
    return (x - in_min) * (out_max - out_min)
           / (in_max - in_min)
           + out_min;
}

static inline int constrain_int(int x, int low, int high){
    if (x < low) return low;
    if (x > high) return high;
    return x;
}

static void init_label_styles(void){

    blue_color = lv_palette_main(LV_PALETTE_CYAN);
    green_color = lv_color_hex(0x28FF00);
    red_color = lv_palette_main(LV_PALETTE_PINK);
    orange_color = lv_palette_main(LV_PALETTE_DEEP_ORANGE);
    purple_color = lv_palette_main(LV_PALETTE_PURPLE);
    pink_color = lv_palette_main(LV_PALETTE_PINK);
}

static void update_label_if_needed(lv_obj_t *label, int new_value, lv_color_t new_color) {
    if (!label) return;  // safety check

    char buf[12];
    snprintf(buf, sizeof(buf), "%d", new_value);

    // Only update text if changed
    const char *old_text = lv_label_get_text(label);
    if (strcmp(old_text, buf) != 0) {
        lv_label_set_text(label, buf);
    }

    // Only update color if changed
    lv_color_t old_color = lv_obj_get_style_text_color(label, LV_PART_MAIN | LV_STATE_DEFAULT);
    if (old_color.full != new_color.full) {
        lv_obj_set_style_text_color(label, new_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

bool isFuelPressureOK() {
    return fabs(g_fuel_pressure - (BASE_FUEL_PRESSURE + g_boost)) <= 4.0;
}

static int get_fake_oscillating_value(void)
{
    uint32_t phase = lv_tick_get() % 2000;
    uint32_t offset = (phase <= 1000) ? phase : (2000 - phase);
    return 70 + (int)((offset * 70 + 500) / 1000);
}

static int get_fake_fuel_level_value(void)
{
    uint32_t phase = lv_tick_get() % 2000;
    uint32_t offset = (phase <= 1000) ? phase : (2000 - phase);
    float t = offset / 1000.0f;
    float eased = 0.5f * (1.0f - cosf(M_PI * t));
    return 5 + (int)(eased * 90.0f + 0.5f);
}

void update_gauge_values(){
    lv_color_t water_temp_color = green_color;
    lv_color_t oil_temp_color = green_color;
    lv_color_t oil_pressure_color = green_color;
    lv_color_t fuel_pressure_color = green_color;

    if (g_use_fake_sensor_data) {
        int fake_value = get_fake_oscillating_value();
        g_water_temp = fake_value;
        g_oil_temp = fake_value;
        g_oil_pressure = fake_value;
        g_fuel_pressure = fake_value;
        g_fuel_level = get_fake_fuel_level_value();
    }

    fuel_pressure_color = (!isFuelPressureOK() || g_fuel_pressure < 30) ? red_color : green_color;
    water_temp_color = (g_water_temp < 150) ? blue_color : ((g_water_temp > 205) ? red_color : green_color);
    oil_temp_color = (g_oil_temp < 150) ? blue_color : ((g_oil_temp > 220) ? red_color : green_color);
    oil_pressure_color = (g_oil_pressure > 125 || g_oil_pressure < 25) ? red_color : green_color;

    update_label_if_needed(ui_Label5, g_water_temp, water_temp_color);
    update_label_if_needed(ui_Label6, g_fuel_pressure, fuel_pressure_color);
    update_label_if_needed(ui_Label7, g_oil_temp, oil_temp_color);
    update_label_if_needed(ui_Label8, g_oil_pressure, oil_pressure_color);

}

void update_fuel_arc(){

    // Update for fuel gauge color, slows down FPS a little
    lv_color_t new_color = green_color;
    new_color = (g_fuel_level < 20) ? red_color : ((g_fuel_level < 35) ? orange_color : green_color) ;

    // Only update color if changed
    lv_color_t old_color = lv_obj_get_style_arc_color(fuel_arc, LV_PART_INDICATOR);
    if (old_color.full != new_color.full) {
        lv_obj_set_style_arc_color(fuel_arc, new_color, LV_PART_INDICATOR);
    }

    if (g_fuel_level < 15) {
        //Blinking handled in low_fuel_blink_timer
    } else if (g_fuel_level < 20) {
        lv_obj_clear_flag(low_gas_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(low_gas_img, LV_OBJ_FLAG_HIDDEN);
    }

    lv_arc_set_value(fuel_arc, g_fuel_level);
}

void low_fuel_blink_timer(lv_timer_t * t) {
    if (g_fuel_level >= 15) {
        return;
    }

    low_fuel_blink_state = !low_fuel_blink_state;

    if (low_fuel_blink_state) {
        lv_obj_clear_flag(low_gas_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(low_gas_img, LV_OBJ_FLAG_HIDDEN);
    }
}

void gauge_timer(lv_timer_t * t) {
    update_gauge_values();
}

void arc_timer(lv_timer_t * t){
    update_fuel_arc();
}


void app_main(void){   
    I2C_Init();
    EXIO_Init();
    LCD_Init();
    Touch_Init();
    LVGL_Init();

    init_label_styles();
    
    ui_init();

    uart_init();

    xTaskCreate(
        uart_rx_task,
        "uart_rx",
        4096,
        NULL,
        10,
        NULL
    );

    lv_timer_create(gauge_timer, 100, NULL);
    lv_timer_create(arc_timer, 20, NULL);
    lv_timer_create(low_fuel_blink_timer, 400, NULL);

    Set_Backlight(0); 
    vTaskDelay(pdMS_TO_TICKS(750)); 
    Set_Backlight(100);
}