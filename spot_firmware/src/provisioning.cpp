#include <Arduino.h>
#include <Preferences.h>
#include "provisioning.h"
#include "config.h"

static uint8_t _pmk[16]        = {};
static uint8_t _spot_id        = 0;
static uint8_t _master_mac[6]  = {};

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
    prefs.begin("spot",   false); prefs.clear(); prefs.end();
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
    Serial.print("[PROV] PMK: ");
    for (int i = 0; i < 16; i++) Serial.printf("%02X", _pmk[i]);
    Serial.println();

    // ── Spot ID ────────────────────────────────────────────────────────────────
    prefs.begin("spot", false);
    if (prefs.isKey("spot_id")) {
        _spot_id = prefs.getUChar("spot_id", CONFIG_SPOT_ID);
        Serial.printf("[PROV] Spot ID loaded from NVS: 0x%02X\n", _spot_id);
    } else {
        _spot_id = CONFIG_SPOT_ID;
        prefs.putUChar("spot_id", _spot_id);
        Serial.printf("[PROV] Spot ID written to NVS from build_flags: 0x%02X\n", _spot_id);
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

    // ── Master MAC ────────────────────────────────────────────────────────────
    // Always write the hardcoded MASTER_MAC from config.h to NVS so the next
    // firmware can read it from NVS and drop the hardcoded value entirely.
    prefs.begin("espnow", false);
    memcpy(_master_mac, MASTER_MAC, 6);
    prefs.putBytes("master_mac", _master_mac, 6);
    Serial.printf("[PROV] Master MAC written to NVS: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  _master_mac[0], _master_mac[1], _master_mac[2],
                  _master_mac[3], _master_mac[4], _master_mac[5]);
    prefs.end();
}

void provisioning_get_pmk(uint8_t pmk_out[16]) {
    memcpy(pmk_out, _pmk, 16);
}

uint8_t provisioning_get_spot_id() {
    return _spot_id;
}

void provisioning_get_master_mac(uint8_t mac_out[6]) {
    memcpy(mac_out, _master_mac, 6);
}

bool provisioning_write(uint8_t spot_id, const char *ssid, const char *password, const char *pmk_hex) {
    uint8_t new_pmk[16];
    if (!hex_to_bytes(pmk_hex, new_pmk, 16)) {
        Serial.println("[PROV] ERROR: invalid PMK hex (need 32 hex chars)");
        return false;
    }

    Preferences prefs;
    prefs.begin("espnow", false); prefs.clear(); prefs.end();
    prefs.begin("wifi",   false); prefs.clear(); prefs.end();
    prefs.begin("spot",   false); prefs.clear(); prefs.end();

    prefs.begin("espnow", false);
    prefs.putBytes("pmk", new_pmk, 16);
    prefs.end();

    prefs.begin("spot", false);
    prefs.putUChar("spot_id", spot_id);
    prefs.end();

    prefs.begin("wifi", false);
    prefs.putString("ssid",     ssid);
    prefs.putString("password", password);
    prefs.end();

    Serial.printf("[PROV] Written: spot_id=0x%02X  ssid=%s\n", spot_id, ssid);
    return true;
}
