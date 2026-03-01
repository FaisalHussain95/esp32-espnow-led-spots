#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "config.h"

// Call once in setup() — initialises ESP-NOW, registers master peer and callbacks.
// WiFi mode is set internally; do not call WiFi.mode() before this.
void espnow_init();

// Sends a status packet to the master node.
void sendStatus(uint8_t brightness, float temperature,
                uint8_t thermal_state, bool is_on);

// Returns true if a new command arrived since the last call.
// Copies the command into *cmd and clears the internal flag.
// Call from loop() only — not ISR-safe on the caller side.
bool espnow_getCommand(esp_now_cmd_t *cmd);
