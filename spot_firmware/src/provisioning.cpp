#include <Arduino.h>
#include <Preferences.h>
#include "provisioning.h"

static uint8_t _pmk[16] = {};

// Convert 32-char lowercase hex string to 16-byte array.
static bool hex_to_bytes(const char *hex, uint8_t *out, size_t out_len) {
    size_t hex_len = strlen(hex);
    if (hex_len != out_len * 2) return false;
    for (size_t i = 0; i < out_len; i++) {
        char hi = hex[i * 2];
        char lo = hex[i * 2 + 1];
        auto hex_val = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int h = hex_val(hi), l = hex_val(lo);
        if (h < 0 || l < 0) return false;
        out[i] = (uint8_t)((h << 4) | l);
    }
    return true;
}

void provisioning_init() {
    Preferences prefs;

#if defined(CONFIG_PROV_FORCE_RESET) && CONFIG_PROV_FORCE_RESET
    // Force-overwrite NVS — use when changing credentials after first flash.
    // Set -DCONFIG_PROV_FORCE_RESET=1 in platformio.ini build_flags, flash once,
    // then remove the flag and reflash to return to normal NVS-first behaviour.
    Serial.println("[PROV] FORCE RESET — clearing NVS credentials.");
    prefs.begin("espnow", false); prefs.clear(); prefs.end();
    prefs.begin("wifi",   false); prefs.clear(); prefs.end();
#endif

    // ── ESP-NOW PMK ────────────────────────────────────────────────────────────
    prefs.begin("espnow", false);
    size_t pmk_len = prefs.getBytesLength("pmk");
    if (pmk_len == 16) {
        prefs.getBytes("pmk", _pmk, 16);
        Serial.println("[PROV] PMK loaded from NVS.");
    } else {
        const char *pmk_hex = CONFIG_ESPNOW_PMK;
        if (!hex_to_bytes(pmk_hex, _pmk, 16)) {
            Serial.println("[PROV] ERROR: CONFIG_ESPNOW_PMK is invalid — check build_flags.");
            while (true) delay(1000);
        }
        prefs.putBytes("pmk", _pmk, 16);
        Serial.println("[PROV] PMK written to NVS from build_flags.");
    }
    prefs.end();

    // ── WiFi credentials (for OTA HTTP) ───────────────────────────────────────
    prefs.begin("wifi", false);
    if (!prefs.isKey("ssid")) {
        prefs.putString("ssid",     CONFIG_WIFI_SSID);
        prefs.putString("password", CONFIG_WIFI_PASSWORD);
        Serial.println("[PROV] WiFi credentials written to NVS from build_flags.");
    } else {
        Serial.println("[PROV] WiFi credentials loaded from NVS.");
    }
    prefs.end();
}

void provisioning_get_pmk(uint8_t pmk_out[16]) {
    memcpy(pmk_out, _pmk, 16);
}
