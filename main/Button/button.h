#pragma once

#include <stdbool.h>

// PTT button on GPIO 0.
//
// Wiring: button between GPIO 0 and GND. The ESP32-S3's internal/external
// strap pull-up holds the line HIGH when released; pressing shorts to GND.
//
// Boot caveat: GPIO 0 is the boot-mode strap pin. If the button is held
// LOW at power-up/reset, the ROM bootloader enters download mode and the
// firmware never starts. Don't hold it at boot.
//
// State changes are debounced (~30 ms) and dispatched from a small
// dedicated task — callbacks run in that task context (NOT in an ISR), so
// they can take FreeRTOS primitives, log via ESP_LOG, etc.
typedef void (*button_state_cb_t)(bool pressed);

void button_init(button_state_cb_t cb);
