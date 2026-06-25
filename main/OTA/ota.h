#pragma once

// Boot-time OTA check against a public GitHub release.
// Call once from post_connect_task after WiFi + Supabase config fetch are
// done and before convai_start. If a newer release is found, downloads the
// .bin asset into the inactive OTA slot and reboots into it; otherwise
// returns and boot continues normally.
//
// May call esp_restart() and never return.
void orb_ota_check_and_update_on_boot(void);

// Call once after a known-good milestone (we use WEBSOCKET_EVENT_CONNECTED
// in convai.c). If the running image is in ESP_OTA_IMG_PENDING_VERIFY state,
// marks it valid and cancels the rollback. Safe to call repeatedly; no-op
// when the image is already valid.
void orb_ota_mark_running_valid(void);
