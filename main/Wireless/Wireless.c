#include "Wireless.h"

#include <string.h>
#include <stdlib.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "orb_ui.h"
#include "config_fetch.h"
#include "i2s_audio.h"
#include "log_sink.h"
#include "secrets.h"
#include "convai.h"
#include "ota.h"

uint16_t BLE_NUM = 0;
uint16_t WIFI_NUM = 0;
bool Scan_finish = 0;

bool WiFi_Scan_Finish = 0;
bool BLE_Scan_Finish = 0;
bool WiFi_Connected = false;

static const char *WIFI_TAG = "orb-wifi";

// ---- WiFi network list (priority order, top = highest) ----
// Defined in secrets.h as WIFI_CREDS. On boot (and after every disconnect) the
// orb scans and connects to the first listed network it can actually see, so a
// missing AP never blocks the boot and switching networks needs no reflash.
typedef struct { const char *ssid; const char *pass; } wifi_cred_t;
static const wifi_cred_t s_creds[] = WIFI_CREDS;
static const size_t s_ncreds = sizeof(s_creds) / sizeof(s_creds[0]);
static int  s_cur_cred = -1;                                  // index being attempted
static bool s_auth_failed[sizeof(s_creds) / sizeof(s_creds[0])] = { false };  // skip this boot (bad pw)
static TaskHandle_t s_wifi_mgr = NULL;                        // runs scan+select off the event task

// Cache of the fetched agent identity, exposed via orb_get_agent_id/name.
// Populated once by post_connect_task after orb_config_fetch returns.
static char s_agent_id[64]   = {0};
static char s_agent_name[64] = {0};

const char *orb_get_agent_id(void)   { return s_agent_id; }
const char *orb_get_agent_name(void) { return s_agent_name; }

bool orb_refresh_config(void)
{
    OrbConfig cfg;
    if (!orb_config_fetch(&cfg)) {
        ESP_LOGW(WIFI_TAG, "config refresh failed — keeping cached agent=%s vol=N/A",
                 s_agent_id);
        return false;
    }
    ESP_LOGI(WIFI_TAG, "config refresh: agent=%s name=%s vol=%d",
             cfg.agent_id, cfg.agent_name, cfg.volume);
    strncpy(s_agent_id,   cfg.agent_id,   sizeof(s_agent_id)   - 1);
    s_agent_id[sizeof(s_agent_id)     - 1] = '\0';
    strncpy(s_agent_name, cfg.agent_name, sizeof(s_agent_name) - 1);
    s_agent_name[sizeof(s_agent_name) - 1] = '\0';
    orb_ui_set_agent_name(cfg.agent_name);
    if (cfg.volume > 0) i2s_audio_set_volume_pct(cfg.volume * 10);
    return true;
}

static void post_connect_task(void *arg)
{
    // UDP log sink FIRST — audio_init has already killed USB-CDC by the
    // time we get here, and anything that crashes between this point and
    // `convai_start` would otherwise be invisible. Catch all of it on UDP.
    log_sink_start(6666);
    ESP_LOGI(WIFI_TAG, "post_connect_task: UDP sink up");

    // SNTP must come before any TLS — cert validation needs the clock.
    orb_sntp_sync(15000);
    ESP_LOGI(WIFI_TAG, "post_connect_task: SNTP done");

    // OTA check runs BEFORE the Supabase config fetch and before anything
    // else that could brick the orb. This makes OTA the absolute recovery
    // floor: as long as the orb can reach WiFi + GitHub, a bad release can
    // be replaced. If we ever ship a build that crashes in orb_refresh_config
    // or convai_start, the orb keeps rebooting INTO this OTA check and will
    // pull the fix. Putting OTA after config would have made that scenario
    // unrecoverable.
    orb_ota_check_and_update_on_boot();
    ESP_LOGI(WIFI_TAG, "post_connect_task: OTA check returned");

    orb_ui_set_state(ORB_CONFIG);

    if (orb_refresh_config()) {
        ESP_LOGI(WIFI_TAG, "post_connect_task: config OK; starting convai");
        // Auto-start the conversation. No idle SPLASH state any more —
        // the device goes straight from CONFIG to LOADING → agent greeting.
        // NFC scans still restart the session (see nfc.c::handle_uid).
        orb_ui_set_state(ORB_LOADING);
        convai_start(s_agent_id);
    } else {
        ESP_LOGE(WIFI_TAG, "config fetch failed");
        // Stay in ORB_CONFIG so the screen reflects that we got the network
        // but never resolved an agent. Manual NFC scan will retry config
        // fetch + convai_start.
    }
    vTaskDelete(NULL);
}

// Scan, then associate to the highest-priority configured network that is in
// range and hasn't failed auth this boot. Single-shot: if none of our networks
// are visible it still attempts the top eligible cred, so the resulting
// NO_AP_FOUND disconnect drives another scan+retry via wifi_event_handler.
// Blocking scan — consistent with the existing blocking retry in that handler.
static void wifi_connect_best(void)
{
    wifi_scan_config_t scan = { .show_hidden = false };
    if (esp_wifi_scan_start(&scan, true) != ESP_OK) {
        ESP_LOGW(WIFI_TAG, "scan start failed");
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found > 20) found = 20;                 // cap the records we pull
    // Heap, NOT stack: wifi_ap_record_t is ~80 B, so recs[20] is ~1.6 KB — far
    // too big for the caller's task stack. (This ran on the 2.3 KB event task
    // originally and silently overflowed it → boot loop.)
    wifi_ap_record_t *recs = found ? calloc(found, sizeof(*recs)) : NULL;
    if (recs) esp_wifi_scan_get_ap_records(&found, recs);
    else      found = 0;

    // Pick the first listed cred that is visible and not auth-failed.
    int chosen = -1;
    for (size_t c = 0; c < s_ncreds && chosen < 0; c++) {
        if (s_auth_failed[c]) continue;
        for (uint16_t r = 0; r < found; r++) {
            if (strcmp(s_creds[c].ssid, (const char *)recs[r].ssid) == 0) {
                chosen = (int)c;
                break;
            }
        }
    }
    free(recs);

    if (chosen >= 0) {
        ESP_LOGI(WIFI_TAG, "selected \"%s\" (priority %d of %u, %u APs seen)",
                 s_creds[chosen].ssid, chosen, (unsigned)s_ncreds, found);
    } else {
        // None in range. Fall back to the top eligible cred so a NO_AP_FOUND
        // disconnect re-drives the scan loop. If every cred has auth-failed,
        // clear the flags and start over rather than lock the orb out.
        for (size_t c = 0; c < s_ncreds; c++) if (!s_auth_failed[c]) { chosen = (int)c; break; }
        if (chosen < 0) {
            for (size_t c = 0; c < s_ncreds; c++) s_auth_failed[c] = false;
            chosen = 0;
        }
        ESP_LOGW(WIFI_TAG, "no configured network in range (%u APs); retrying via \"%s\"",
                 found, s_creds[chosen].ssid);
    }

    s_cur_cred = chosen;
    wifi_config_t wcfg = {0};
    strncpy((char *)wcfg.sta.ssid,     s_creds[chosen].ssid, sizeof(wcfg.sta.ssid) - 1);
    strncpy((char *)wcfg.sta.password, s_creds[chosen].pass, sizeof(wcfg.sta.password) - 1);
    // Accept open..WPA3 (WPA3-only routers reject the stricter WPA2 threshold).
    wcfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wcfg.sta.pmf_cfg.capable  = true;
    wcfg.sta.pmf_cfg.required = false;
    esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    esp_wifi_connect();
}

// Runs the blocking scan+select off the tiny (~2.3 KB) WiFi event-handler task,
// which cannot afford it. The event handler only nudges this task; a short
// settle delay keeps a reconnect storm from hammering the radio.
static void wifi_manager_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   // wait for a (re)connect nudge
        vTaskDelay(pdMS_TO_TICKS(500));            // let disconnect churn settle
        wifi_connect_best();
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        orb_ui_set_state(ORB_WIFI);
        xTaskNotifyGive(s_wifi_mgr);            // scan+connect runs on wifi_mgr task
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        WiFi_Connected = false;
        orb_ui_set_state(ORB_WIFI);
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
        uint8_t reason = ev ? ev->reason : 0;
        // reason codes: 201=NO_AP_FOUND (out of range / wrong SSID / 5GHz-only),
        // 202=AUTH_FAIL (wrong password), 15=4WAY_HANDSHAKE_TIMEOUT (also usually
        // wrong PW), 204=HANDSHAKE_TIMEOUT, 200=AUTH_EXPIRE.
        ESP_LOGW(WIFI_TAG, "disconnected reason=%d (cred %d), rescanning", reason, s_cur_cred);
        // A wrong-password / handshake failure means this network is present but
        // unusable — skip it for the rest of the boot so we don't loop on it.
        // NO_AP_FOUND is just out-of-range; keep it eligible for the next scan.
        if (s_cur_cred >= 0 &&
            (reason == WIFI_REASON_AUTH_FAIL ||
             reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
             reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
             reason == WIFI_REASON_AUTH_EXPIRE)) {
            s_auth_failed[s_cur_cred] = true;
            ESP_LOGW(WIFI_TAG, "auth failed for \"%s\"; skipping it this boot",
                     s_creds[s_cur_cred].ssid);
        }
        xTaskNotifyGive(s_wifi_mgr);            // re-scan+connect on wifi_mgr task
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(WIFI_TAG, "got IP " IPSTR, IP2STR(&ev->ip_info.ip));
        WiFi_Connected = true;
        // CONFIG state is set inside post_connect_task, AFTER the WIFI→
        // CONFIG transition beep. UI stays on ORB_WIFI until then.
        // 8 KB stack: post_connect_task now does TWO back-to-back TLS handshakes
        // (Supabase config + GitHub /releases/latest) plus esp_https_ota's own
        // setup if an update is found. 6 KB was tight for one handshake; two is
        // unsafe. Internal SRAM is still healthy at this point — Convai's PSRAM
        // buffers haven't been allocated yet.
        xTaskCreatePinnedToCore(post_connect_task, "orb_postcon", 8192, NULL, 4, NULL, 0);
    }
}

void Wireless_Init(void)
{
    // Initialize NVS.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );
    // WiFi
    xTaskCreatePinnedToCore(
        WIFI_Init,
        "WIFI task",
        4096,
        NULL,
        2,
        NULL,
        0);
    // BLE — preserved but NOT auto-started. Starting BLE_Init at boot lets
    // the BLE scan steal the radio from WiFi association via the coex
    // arbiter ("Coexist: Wi-Fi connect fail"). When BLE features come
    // online, spawn this task explicitly from wherever needs it AFTER the
    // WiFi connection is stable.
    //
    // xTaskCreatePinnedToCore(BLE_Init, "BLE task", 4096, NULL, 2, NULL, 0);
}

void WIFI_Init(void *arg)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // Per-network config is applied later in wifi_connect_best() (on STA_START),
    // after a scan picks the highest-priority network actually in range. That
    // runs on this dedicated 4 KB task — NOT the ~2.3 KB event-handler task,
    // which the scan-results buffer overflows. Must exist before esp_wifi_start()
    // so the STA_START nudge lands.
    xTaskCreatePinnedToCore(wifi_manager_task, "wifi_mgr", 4096, NULL, 3, &s_wifi_mgr, 0);

    ESP_ERROR_CHECK(esp_wifi_start());

    vTaskDelete(NULL);
}

uint16_t WIFI_Scan(void)
{
    uint16_t ap_count = 0;
    esp_wifi_scan_start(NULL, true);
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
    WiFi_Scan_Finish = 1;
    if (BLE_Scan_Finish == 1)
        Scan_finish = 1;
    return ap_count;
}


#define GATTC_TAG "GATTC_TAG"
#define SCAN_DURATION 20  
#define MAX_DISCOVERED_DEVICES 100 

typedef struct {
    uint8_t address[6];
    bool is_valid;
} discovered_device_t;

static discovered_device_t discovered_devices[MAX_DISCOVERED_DEVICES];
static size_t num_discovered_devices = 0;
static size_t num_devices_with_name = 0;


static bool is_device_discovered(const uint8_t *addr) {
    for (size_t i = 0; i < num_discovered_devices; i++) {
        if (memcmp(discovered_devices[i].address, addr, 6) == 0) {
            return true;
        }
    }
    return false;
}


static void add_device_to_list(const uint8_t *addr) {
    if (num_discovered_devices < MAX_DISCOVERED_DEVICES) {
        memcpy(discovered_devices[num_discovered_devices].address, addr, 6);
        discovered_devices[num_discovered_devices].is_valid = true;
        num_discovered_devices++;
    }
}

static bool extract_device_name(const uint8_t *adv_data, uint8_t adv_data_len, char *device_name, size_t max_name_len) {
    size_t offset = 0;
    while (offset < adv_data_len) {
        if (adv_data[offset] == 0) break;

        uint8_t length = adv_data[offset];
        if (length == 0 || offset + length > adv_data_len) break; 

        uint8_t type = adv_data[offset + 1];
        if (type == ESP_BLE_AD_TYPE_NAME_CMPL || type == ESP_BLE_AD_TYPE_NAME_SHORT) {
            if (length > 1 && length - 1 < max_name_len) {
                memcpy(device_name, &adv_data[offset + 2], length - 1);
                device_name[length - 1] = '\0'; 
                return true;
            } else {
                return false;
            }
        }
        offset += length + 1;
    }
    return false;
}

static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    static char device_name[100];

    switch (event) {
        case ESP_GAP_BLE_SCAN_RESULT_EVT:
            if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
                if (!is_device_discovered(param->scan_rst.bda)) {
                    add_device_to_list(param->scan_rst.bda);
                    BLE_NUM++; 

                    if (extract_device_name(param->scan_rst.ble_adv, param->scan_rst.adv_data_len, device_name, sizeof(device_name))) {
                        num_devices_with_name++;
                        // printf("Found device: %02X:%02X:%02X:%02X:%02X:%02X\n        Name: %s\n        RSSI: %d\r\n",
                        //          param->scan_rst.bda[0], param->scan_rst.bda[1],
                        //          param->scan_rst.bda[2], param->scan_rst.bda[3],
                        //          param->scan_rst.bda[4], param->scan_rst.bda[5],
                        //          device_name, param->scan_rst.rssi);
                        // printf("\r\n");
                    } else {
                        // printf("Found device: %02X:%02X:%02X:%02X:%02X:%02X\n        Name: Unknown\n        RSSI: %d\r\n",
                        //          param->scan_rst.bda[0], param->scan_rst.bda[1],
                        //          param->scan_rst.bda[2], param->scan_rst.bda[3],
                        //          param->scan_rst.bda[4], param->scan_rst.bda[5],
                        //          param->scan_rst.rssi);
                        // printf("\r\n");
                    }
                }
            }
            break;
        case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
            ESP_LOGI(GATTC_TAG, "Scan complete. Total devices found: %d (with names: %d)", BLE_NUM, num_devices_with_name);
            break;
        default:
            break;
    }
}

void BLE_Init(void *arg)
{
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);                                            
    if (ret) {
        printf("%s initialize controller failed: %s\n", __func__, esp_err_to_name(ret));        
        return;}
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);                                           
    if (ret) {
        printf("%s enable controller failed: %s\n", __func__, esp_err_to_name(ret));            
        return;}
    ret = esp_bluedroid_init();                                                                 
    if (ret) {
        printf("%s init bluetooth failed: %s\n", __func__, esp_err_to_name(ret));               
        return;}
    ret = esp_bluedroid_enable();                                                               
    if (ret) {
        printf("%s enable bluetooth failed: %s\n", __func__, esp_err_to_name(ret));             
        return;}

    //register the  callback function to the gap module
    ret = esp_ble_gap_register_callback(esp_gap_cb);                                            
    if (ret){
        printf("%s gap register error, error code = %x\n", __func__, ret);                      
        return;
    }
    BLE_Scan();
    vTaskDelete(NULL);

}
uint16_t BLE_Scan(void)
{
    esp_ble_scan_params_t scan_params = {
        .scan_type = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type = BLE_ADDR_TYPE_RPA_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = 0x50,   
        .scan_window = 0x30,       
        .scan_duplicate         = BLE_SCAN_DUPLICATE_DISABLE
    };
    ESP_ERROR_CHECK(esp_ble_gap_set_scan_params(&scan_params));

    printf("Starting BLE scan...\n");
    ESP_ERROR_CHECK(esp_ble_gap_start_scanning(SCAN_DURATION));
    
    // Set scanning duration
    vTaskDelay(SCAN_DURATION * 1000 / portTICK_PERIOD_MS);
    
    printf("Stopping BLE scan...\n");
    ESP_ERROR_CHECK(esp_ble_gap_stop_scanning());
    BLE_Scan_Finish = 1;
    if(WiFi_Scan_Finish == 1)
        Scan_finish = 1;
    return BLE_NUM;
}