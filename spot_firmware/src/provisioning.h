#pragma once

// Call once at top of setup(), before espnow_init().
// Reads PMK and WiFi credentials from Kconfig (menuconfig) and writes them
// to NVS on first boot. Subsequent boots return instantly (NVS already populated).
// NVS survives OTA flashes — credentials persist across firmware updates.
void provisioning_init();

// Returns the 16-byte PMK loaded from NVS into the provided buffer.
// Must be called after provisioning_init().
void provisioning_get_pmk(uint8_t pmk_out[16]);
