#pragma once

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "nvs_flash.h" 
#include "esp_log.h"

#include <stdio.h>
#include <string.h>  // For memcpy
#include "esp_system.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_bt_main.h"



extern uint16_t BLE_NUM;
extern uint16_t WIFI_NUM;
extern bool Scan_finish;

void Wireless_Init(void);
void WIFI_Init(void *arg);
uint16_t WIFI_Scan(void);
void BLE_Init(void *arg);
uint16_t BLE_Scan(void);

// True once esp_wifi has an IP. Useful for gating HTTPS calls.
extern bool WiFi_Connected;

// Populated once orb_config_fetch succeeds. Empty string until then.
// Read-only — modify only from post_connect_task / orb_refresh_config.
const char *orb_get_agent_id(void);
const char *orb_get_agent_name(void);

// Re-runs orb_config_fetch and applies the result (updates cached
// agent_id / agent_name / orb_ui agent label / I2S TX volume). Safe to
// call from any task. Returns true if the fetch succeeded.
bool orb_refresh_config(void);