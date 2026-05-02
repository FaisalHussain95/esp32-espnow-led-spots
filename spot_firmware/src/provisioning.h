#pragma once

// Call once at top of setup(), before espnow_init().
// Reads PMK and WiFi credentials from Kconfig (menuconfig) and writes them
// to NVS on first boot. Subsequent boots return instantly (NVS already populated).
// NVS survives OTA flashes — credentials persist across firmware updates.
void provisioning_init();

// Returns the 16-byte PMK loaded from NVS into the provided buffer.
// Must be called after provisioning_init().
void provisioning_get_pmk(uint8_t pmk_out[16]);

// Returns the spot ID loaded from NVS (default: CONFIG_SPOT_ID build flag).
// Must be called after provisioning_init().
uint8_t provisioning_get_spot_id();

// Overwrite all NVS credentials from arguments. Clears existing values first.
// pmk_hex must be a 32-char hex string (e.g. "a1b2c3d4...").
// Returns false and prints an error if pmk_hex is invalid — NVS is not touched.
// Call ESP.restart() after a successful write to apply the new values.
bool provisioning_write(uint8_t spot_id, const char *ssid, const char *password, const char *pmk_hex);

// Returns the master MAC loaded from NVS into the provided 6-byte buffer.
// Must be called after provisioning_init().
void provisioning_get_master_mac(uint8_t mac_out[6]);
