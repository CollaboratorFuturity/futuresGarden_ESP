#include <stdio.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "TCA9554PWR.h"
#include "ST7701S.h"
#include "SD_MMC.h"
#include "LVGL_Driver.h"
#include "orb_ui.h"
#include "Wireless.h"
#include "nfc.h"
#include "BAT_Driver.h"
#include "i2s_audio.h"
#include "button.h"
#include "convai.h"

static const char *MAIN_TAG = "main";

// ─── Diagnostic log verbosity ────────────────────────────────────────────
// 1 = show the per-2s convai heartbeat ("hb: ws=up …") and the per-ping
//     "rx: type=ping" spam. Useful when debugging connection health.
// 0 = quiet. Connect/disconnect, errors, PTT, and user/agent transcripts
//     are ALWAYS logged regardless of this flag.
#define ORB_VERBOSE_CONVAI_LOGS 0

static void on_button_state(bool pressed)
{
    ESP_LOGI(MAIN_TAG, "button %s", pressed ? "DOWN" : "UP");
    // PTT — hold to talk. If no conversation is running, convai_ptt_set is
    // a no-op (so pressing the button at SPLASH does nothing yet).
    convai_ptt_set(pressed);
}

void Driver_Loop(void *parameter)
{
    while(1)
    {
        // Sample every 100 ms. NOTE: this firmware has no idle state — it boots
        // straight into a persistent conversation (Wireless.c post_connect_task),
        // so we can't gate sampling to an idle window. The ~190 mV harness IR drop
        // under load makes this read a bit low; the moving average in
        // BAT_Get_Volts keeps it steady. Recalibrate Measurement_offset against
        // the loaded path if the absolute value matters.
        BAT_Get_Volts();
        orb_ui_set_battery(BAT_analogVolts);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelete(NULL);
}
void Driver_Init(void)
{
    Flash_Searching();
    BAT_Init();
    I2C_Init();
    EXIO_Init();
    xTaskCreatePinnedToCore(
        Driver_Loop,
        "Other Driver task",
        4096,
        NULL,
        3,
        NULL,
        0);
}
void app_main(void)
{
    // PN532 nacks every "no tag present" status poll; the IDF 5.5
    // i2c.master driver logs each one at ERROR. Tag reads still work —
    // mute the noise so the UDP log isn't drowned.
    esp_log_level_set("i2c.master", ESP_LOG_NONE);

    // Heartbeat / ping-spam verbosity — flip ORB_VERBOSE_CONVAI_LOGS above.
    convai_set_log_verbose(ORB_VERBOSE_CONVAI_LOGS);

    // I2C + EXIO must come up first — LCD and SD reach into the TCA9554
    // during their own init, so the bus has to exist before they run.
    Driver_Init();

    LCD_Init();
    SD_Init();
    LVGL_Init();

    // The LVGL task runs continuously inside LVGL_Driver.c. Code that
    // touches LVGL objects directly must hold the LVGL mutex. Background
    // tasks (battery/state) only update atomics, so they don't need it.
    if (LVGL_Lock(0)) {
        orb_ui_init();
        LVGL_Unlock();
    }
    orb_ui_set_state(ORB_BOOT);

    // PN532 lives on the same I2C bus (J8, addr 0x24). Connect to the header
    // marked I2C (NOT the UART header). DIP switches: SW1=ON, SW2=OFF.
    // The PN532 task itself runs the record→playback loopback on every tag
    // scan (see nfc.c) — that's our mic test path.
    NFC_Init();

    // PTT button on GPIO 0 (button to GND). Held → mic streams to the
    // ElevenLabs agent. Released → mic stops; server VAD treats the gap as
    // end-of-turn and the agent responds.
    button_init(on_button_state);

    // Bring audio up here (BEFORE WiFi). The "ready" beep plays at the
    // WIFI→CONFIG transition (see post_connect_task). Heads up: this
    // claims GPIO 19/20/43/44 and kills USB-CDC + the UART bridge — boot
    // logs above this line are still visible, anything after needs UDP
    // (which only comes up once WiFi connects via post_connect_task).
    i2s_audio_init();

    // Wireless drives BOOT→WIFI→CONFIG→SPLASH via its event handler and
    // the post-connect task that runs SNTP + Supabase config fetch.
    Wireless_Init();
}
