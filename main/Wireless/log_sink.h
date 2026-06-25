#pragma once

#include <stdint.h>

// UDP log sink — redirects esp_log output to a UDP broadcast on port `port`.
// Call once WiFi is connected. After this, every ESP_LOGI/W/E is sent as a
// UDP packet to 255.255.255.255:<port> AND printed to UART (which is dead
// once audio inits).
//
// On the host:
//     nc -u -l <port>          # macOS / Linux
//
// Must be on the same WiFi subnet. If broadcast is blocked by the AP, edit
// LOG_SINK_TARGET_IP below.
void log_sink_start(uint16_t port);
