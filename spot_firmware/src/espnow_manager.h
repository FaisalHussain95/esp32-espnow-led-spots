#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "config.h"

// Call once in setup() after provisioning_init().
// Loads PMK from NVS, enables encryption, registers master peer, sends MSG_HELLO.
void espnow_init();

// Sends a MSG_STATUS packet to the master node.
// msg_type defaults to MSG_STATUS; pass MSG_OTA_FAILED for OTA failure reports.
void sendStatus(uint8_t brightness, float temperature,
                uint8_t thermal_state, bool is_on,
                uint8_t msg_type = MSG_STATUS, uint8_t attempt = 0);

// Returns true if a new command arrived since the last call.
// Copies the inner esp_now_cmd_t into *cmd and clears the internal flag.
// Call from loop() only.
bool espnow_getCommand(esp_now_cmd_t *cmd);

// Returns true if an OTA trigger arrived. Fills target_version.
bool espnow_getOtaTrigger(uint8_t *target_version);
