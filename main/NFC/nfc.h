#pragma once

#include <stdbool.h>

// Starts the PN532 polling task (handles its own init retry). Call after
// I2C_Init() — the PN532 sits on the same I2C bus as the TCA9554 at addr 0x24.
void NFC_Init(void);

// Enable/disable tag polling. The WS audio-capture path will call this to
// suppress reads during user turns. Default is enabled.
void NFC_Set_Polling(bool enable);
