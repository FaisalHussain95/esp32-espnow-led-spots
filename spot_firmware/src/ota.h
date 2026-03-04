#pragma once
#include <stdint.h>

// Fetch and apply firmware from GitHub release.
// Connects to WiFi (using NVS credentials), downloads binary, flashes, restarts.
// On failure: retries up to OTA_MAX_ATTEMPTS times (OTA_RETRY_DELAY_MS between each),
// notifies master of each failure via ESP-NOW, then returns (stays alive on old firmware).
void ota_start(uint8_t target_version);
