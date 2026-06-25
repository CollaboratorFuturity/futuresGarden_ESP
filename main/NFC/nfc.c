#include "nfc.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"

#include "I2C_Driver.h"
#include "orb_ui.h"
#include "i2s_audio.h"
#include "convai.h"
#include "Wireless.h"

#define PN532_ADDR        0x24
#define PN532_FREQ_HZ     400000   // PN532 datasheet supports up to 400 kHz I2C
#define PN532_TAG         "nfc"

// PN532 host commands
#define CMD_GETFIRMWAREVERSION  0x02
#define CMD_SAMCONFIGURATION    0x14
#define CMD_RFCONFIGURATION     0x32
#define CMD_INLISTPASSIVETARGET 0x4A

#define TFI_HOST_TO_PN532 0xD4
#define TFI_PN532_TO_HOST 0xD5

#define POLL_PERIOD_MS    100        // matches working ESP-IDF PN532 libs
#define DEBOUNCE_US       1500000   // 1.5 s same-UID debounce

// (Old audio loopback test is gone; scans now hand off to convai_start.
//  The Convai module owns its own audio buffers.)

static i2c_master_dev_handle_t s_dev = NULL;

static volatile bool s_polling_enabled = true;
static uint8_t s_last_uid[10];
static int s_last_uid_len = 0;
static int64_t s_last_seen_us = 0;


// ─── Low-level frame helpers ─────────────────────────────────────────────

static esp_err_t pn532_write_command(const uint8_t *body, uint8_t body_len)
{
    // Frame: 00 00 FF LEN LCS TFI body... DCS 00
    // LEN = TFI + body_len ; LCS = -LEN mod 256 ; DCS = -(TFI + sum(body)) mod 256
    uint8_t frame[32];
    if (body_len > sizeof(frame) - 8) return ESP_ERR_INVALID_SIZE;

    uint8_t len = body_len + 1;
    uint8_t lcs = (uint8_t)(0x100 - len);
    uint8_t dcs = TFI_HOST_TO_PN532;
    for (int i = 0; i < body_len; i++) dcs += body[i];
    dcs = (uint8_t)(0x100 - dcs);

    int idx = 0;
    frame[idx++] = 0x00;
    frame[idx++] = 0x00;
    frame[idx++] = 0xFF;
    frame[idx++] = len;
    frame[idx++] = lcs;
    frame[idx++] = TFI_HOST_TO_PN532;
    memcpy(&frame[idx], body, body_len);
    idx += body_len;
    frame[idx++] = dcs;
    frame[idx++] = 0x00;

    return i2c_master_transmit(s_dev, frame, idx, 100);
}

// Poll the PN532's ready-status byte until it reports ready (bit 0 set) or
// the timeout elapses. PN532 I2C reads always have a 1-byte status prefix.
// Tight 2 ms polling step — PN532 is usually ready in 1-3 ms; the previous
// 10 ms step was wasting most of the wait window.
static esp_err_t pn532_wait_ready(int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        uint8_t rdy = 0;
        if (i2c_master_receive(s_dev, &rdy, 1, 20) == ESP_OK && (rdy & 0x01)) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
        waited += 2;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t pn532_read_ack(void)
{
    // Allow up to 200 ms for the ACK (was 50 ms — too tight under load).
    if (pn532_wait_ready(200) != ESP_OK) return ESP_ERR_TIMEOUT;
    uint8_t buf[7];  // 1 status byte + 6-byte ACK
    esp_err_t err = i2c_master_receive(s_dev, buf, sizeof(buf), 100);
    if (err != ESP_OK) return err;
    static const uint8_t expected[6] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};
    return memcmp(&buf[1], expected, 6) == 0 ? ESP_OK : ESP_FAIL;
}

// Read a response frame into `out`. Returns data length (excluding TFI) or -1.
static int pn532_read_response(uint8_t *out, int max_len)
{
    // 500 ms covers slow responses under load. With MxRtyPassiveActivation
    // = 1, a typical no-card response is 30–60 ms; with-card ~10–30 ms.
    if (pn532_wait_ready(500) != ESP_OK) return -1;
    uint8_t buf[64];
    int want = max_len + 9;
    if (want > (int)sizeof(buf)) want = sizeof(buf);
    if (i2c_master_receive(s_dev, buf, want, 100) != ESP_OK) return -1;

    // buf[0] = ready byte; skip 0x00 preamble bytes
    int i = 1;
    while (i < want && buf[i] == 0x00) i++;
    if (i >= want || buf[i] != 0xFF) return -1;
    i++;
    if (i + 2 >= want) return -1;
    uint8_t len = buf[i++];
    uint8_t lcs = buf[i++];
    if ((uint8_t)(len + lcs) != 0) return -1;
    if (i >= want || buf[i++] != TFI_PN532_TO_HOST) return -1;

    int data_len = (int)len - 1;
    if (data_len < 0 || data_len > max_len || i + data_len > want) return -1;
    memcpy(out, &buf[i], data_len);
    return data_len;
}

// ─── High-level commands ─────────────────────────────────────────────────

static bool pn532_get_firmware_version(void)
{
    uint8_t cmd[] = {CMD_GETFIRMWAREVERSION};
    if (pn532_write_command(cmd, sizeof(cmd)) != ESP_OK) return false;
    if (pn532_read_ack() != ESP_OK) return false;
    uint8_t resp[8];
    int n = pn532_read_response(resp, sizeof(resp));
    if (n < 5 || resp[0] != (CMD_GETFIRMWAREVERSION + 1)) return false;
    ESP_LOGI(PN532_TAG, "PN5%02X v%d.%d support=0x%02X", resp[1], resp[2], resp[3], resp[4]);
    return true;
}

static bool pn532_sam_configuration(void)
{
    // Normal mode, 1 s timeout, no IRQ pin.
    uint8_t cmd[] = {CMD_SAMCONFIGURATION, 0x01, 0x14, 0x01};
    if (pn532_write_command(cmd, sizeof(cmd)) != ESP_OK) return false;
    if (pn532_read_ack() != ESP_OK) return false;
    uint8_t resp[4];
    int n = pn532_read_response(resp, sizeof(resp));
    return n >= 1 && resp[0] == (CMD_SAMCONFIGURATION + 1);
}

// RFConfiguration → CfgItem 0x05 (MaxRetries): bytes are MxRtyATR,
// MxRtyPSL, MxRtyPassiveActivation. The default MxRtyPassiveActivation is
// 0xFF (infinite) which makes InListPassiveTarget block until a card shows
// up — and our 100 ms response timeout then fires every poll, leaving the
// chip in a half-busy state that's flaky to recover from. Setting it to
// 0x01 (one retry = up to ~50 ms attempt) makes InListPassiveTarget return
// promptly with NbTg=0 when no card is in the field. Keeping ATR and PSL
// at typical values.
static bool pn532_set_passive_retries(uint8_t passive_retries)
{
    uint8_t cmd[] = {CMD_RFCONFIGURATION, 0x05, 0xFF, 0x01, passive_retries};
    if (pn532_write_command(cmd, sizeof(cmd)) != ESP_OK) return false;
    if (pn532_read_ack() != ESP_OK) return false;
    uint8_t resp[2];
    int n = pn532_read_response(resp, sizeof(resp));
    return n >= 1 && resp[0] == (CMD_RFCONFIGURATION + 1);
}

// Returns UID length (4 or 7) on detection, 0 if no card, -1 on protocol error.
static int pn532_read_passive_target(uint8_t *uid, int max_uid)
{
    // MaxTg=1, BrTy=0x00 (ISO14443A @ 106 kbps)
    uint8_t cmd[] = {CMD_INLISTPASSIVETARGET, 0x01, 0x00};
    if (pn532_write_command(cmd, sizeof(cmd)) != ESP_OK) return -1;
    if (pn532_read_ack() != ESP_OK) return -1;

    uint8_t resp[32];
    int n = pn532_read_response(resp, sizeof(resp));
    if (n < 2 || resp[0] != (CMD_INLISTPASSIVETARGET + 1)) return -1;
    if (resp[1] == 0) return 0;  // no target in field

    // Layout: 4B NbTg, Tg, SENS_RES(2), SEL_RES, NFCIDLen, NFCID...
    if (n < 7) return -1;
    int uid_len = resp[6];
    if (uid_len <= 0 || uid_len > max_uid || n < 7 + uid_len) return -1;
    memcpy(uid, &resp[7], uid_len);
    return uid_len;
}

// ─── Tag handling ─────────────────────────────────────────────────────────

static void uid_to_hex(const uint8_t *uid, int len, char *out, int out_len)
{
    int idx = 0;
    for (int i = 0; i < len && idx + 3 < out_len; i++) {
        idx += snprintf(&out[idx], out_len - idx, "%02X", uid[i]);
    }
}

static void handle_uid(const uint8_t *uid, int len)
{
    int64_t now = esp_timer_get_time();

    bool same = (len == s_last_uid_len) && (memcmp(uid, s_last_uid, len) == 0);
    if (same && (now - s_last_seen_us) < DEBOUNCE_US) {
        s_last_seen_us = now;
        return;
    }
    memcpy(s_last_uid, uid, len);
    s_last_uid_len = len;
    s_last_seen_us = now;

    char hex[24] = {0};
    uid_to_hex(uid, len, hex, sizeof(hex));
    ESP_LOGI(PN532_TAG, "tag UID=%s (%d bytes)", hex, len);

    // Scan acknowledgement: show ORB_NFC first so the screen is already on the
    // purple "NFC Scanned" state while the blopp plays. It stays up through the
    // config fetch; the next real state (ORB_LOADING before convai_start, then
    // the Convai tasks' USER_TALK / AGENT / MUTED) replaces it naturally.
    orb_ui_set_state(ORB_NFC);

    // Pre-Convai cue: three-tone "blopp" so the user hears that the tag was
    // registered before the agent starts talking.
    static const i2s_audio_tone_t kTagBlopp[] = {
        { 250, 80 },
        { 290, 80 },
        { 350, 120 },
    };
    i2s_audio_tone_sequence(kTagBlopp, sizeof(kTagBlopp) / sizeof(kTagBlopp[0]));

    // Test-mode: any tag starts a conversation with the agent fetched at
    // boot. Whitelist comes later (stage 5). If the conversation is already
    // Re-scanning the tag while a conversation is running restarts it.
    // (Stage 5 will replace this with the real tag table — AGENT_START vs
    // TEST vs custom phrase per BEHAVIOR.md §6.)
    if (convai_is_running()) {
        ESP_LOGI(PN532_TAG, "tag re-scan during running_agent — restarting conversation");
        convai_stop();
        vTaskDelay(pdMS_TO_TICKS(200));   // let teardown settle (ORB_NFC stays up)
    }
    // Refresh config before each start so agent / volume changes from
    // Supabase land without a reboot. Failure is non-fatal — we keep the
    // cached values from the previous fetch. ORB_NFC remains on screen for
    // the duration of this TLS HTTPS GET.
    orb_refresh_config();
    const char *agent = orb_get_agent_id();
    if (!agent || !agent[0]) {
        ESP_LOGW(PN532_TAG, "no agent_id (config never fetched) — can't start convai");
        orb_ui_set_state(ORB_LOW_BAT);
        vTaskDelay(pdMS_TO_TICKS(1000));
        orb_ui_set_state(ORB_MUTED);
        return;
    }
    orb_ui_set_state(ORB_LOADING);   // yellow + sweep: WSS connecting
    convai_start(agent);
}

// ─── Lifecycle ───────────────────────────────────────────────────────────

static bool pn532_bringup(void)
{
    if (!pn532_get_firmware_version()) return false;
    if (!pn532_sam_configuration())    { ESP_LOGW(PN532_TAG, "SAMConfiguration failed"); return false; }
    if (!pn532_set_passive_retries(0x01)) {
        ESP_LOGW(PN532_TAG, "RFConfiguration MxRtyPassive failed — InListPassiveTarget will block");
        return false;
    }
    return true;
}

static void nfc_task(void *arg)
{
    // Open device handle on the existing I2C bus.
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = PN532_ADDR,
        .scl_speed_hz    = PN532_FREQ_HZ,
    };
    if (i2c_master_bus_add_device(i2c_bus_handle, &cfg, &s_dev) != ESP_OK) {
        ESP_LOGE(PN532_TAG, "i2c_master_bus_add_device failed");
        vTaskDelete(NULL);
        return;
    }

    // Initial init: 5 attempts, 500 ms apart.
    bool up = false;
    for (int i = 0; i < 5 && !up; i++) {
        if (pn532_bringup()) { up = true; break; }
        ESP_LOGW(PN532_TAG, "init attempt %d/5 failed", i + 1);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    // Long-cycle recovery — don't silently die.
    while (!up) {
        ESP_LOGE(PN532_TAG, "PN532 not responding (check DIP switches: SW1=ON, SW2=OFF). Retrying in 30s.");
        vTaskDelay(pdMS_TO_TICKS(30000));
        up = pn532_bringup();
    }
    ESP_LOGI(PN532_TAG, "PN532 ready, polling every %d ms", POLL_PERIOD_MS);

    // Heartbeat: every ~10 s, log a summary so the UDP sink shows the task
    // is alive and how often pn532_read_passive_target is failing vs
    // returning "no card". Reset on a successful scan.
    int polls = 0, no_card = 0, errors = 0;
    int64_t last_hb_us = esp_timer_get_time();

    while (1) {
        if (s_polling_enabled) {
            uint8_t uid[10];
            int n = pn532_read_passive_target(uid, sizeof(uid));
            polls++;
            if (n > 0) {
                handle_uid(uid, n);
            } else if (n == 0) {
                no_card++;
            } else {
                errors++;
            }
        }
        int64_t now = esp_timer_get_time();
        if (now - last_hb_us >= 10000000) {
            // Heartbeat silenced now that polling is stable. Re-enable if
            // I2C errors / PN532 issues come back.
            // ESP_LOGI(PN532_TAG, "poll heartbeat: polls=%d no_card=%d errors=%d",
            //          polls, no_card, errors);
            polls = no_card = errors = 0;
            last_hb_us = now;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

void NFC_Init(void)
{
    // 8 KB stack: the handle_uid path now does orb_refresh_config() →
    // HTTPS GET → TLS handshake, which mbedTLS sinks ~5 KB of stack into.
    // 4 KB was enough for the old PSRAM-buffer loopback test but overflows
    // here.
    xTaskCreatePinnedToCore(nfc_task, "nfc", 8192, NULL, 3, NULL, 0);
}

void NFC_Set_Polling(bool enable)
{
    s_polling_enabled = enable;
}
