#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include "ota.h"
#include "config.h"
#include "espnow_manager.h"

static void notify_master_ota_failed(uint8_t attempt) {
    // Send MSG_OTA_FAILED so master/OLED/HA are informed
    sendStatus(0, -1.0f, THERMAL_NORMAL, false, MSG_OTA_FAILED, attempt);
}

void ota_start(uint8_t target_version) {
    Serial.printf("[OTA] Starting — target version %d\n", target_version);

    // Load WiFi credentials from NVS
    Preferences prefs;
    prefs.begin("wifi", true);
    String ssid     = prefs.getString("ssid",     "");
    String password = prefs.getString("password", "");
    prefs.end();

    if (ssid.isEmpty()) {
        Serial.println("[OTA] ERROR: No WiFi credentials in NVS. Re-provision.");
        return;
    }

    // Connect to WiFi
    Serial.printf("[OTA] Connecting to WiFi: %s", ssid.c_str());
    WiFi.begin(ssid.c_str(), password.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
        delay(500);
        Serial.print(".");
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\n[OTA] WiFi connect failed.");
        notify_master_ota_failed(0);
        return;
    }
    Serial.printf("\n[OTA] WiFi OK. IP: %s\n", WiFi.localIP().toString().c_str());

    // Build URL
    char url[192];
    snprintf(url, sizeof(url), OTA_BASE_URL, target_version, target_version);
    Serial.printf("[OTA] URL: %s\n", url);

    // Attempt OTA with retry
    for (uint8_t attempt = 1; attempt <= OTA_MAX_ATTEMPTS; attempt++) {
        Serial.printf("[OTA] Attempt %d/%d...\n", attempt, OTA_MAX_ATTEMPTS);

        WiFiClientSecure client;
        client.setInsecure();  // skip cert validation (GitHub CDN)
        httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        t_httpUpdate_return result = httpUpdate.update(client, url);

        switch (result) {
            case HTTP_UPDATE_FAILED:
                Serial.printf("[OTA] Attempt %d failed: %s\n",
                              attempt, httpUpdate.getLastErrorString().c_str());
                notify_master_ota_failed(attempt);
                if (attempt < OTA_MAX_ATTEMPTS) {
                    Serial.printf("[OTA] Retrying in %ds...\n", OTA_RETRY_DELAY_MS / 1000);
                    delay(OTA_RETRY_DELAY_MS);
                }
                break;

            case HTTP_UPDATE_NO_UPDATES:
                Serial.println("[OTA] Server says no update available.");
                notify_master_ota_failed(attempt);
                return;  // No point retrying

            case HTTP_UPDATE_OK:
                // httpUpdate automatically restarts the device after flashing
                Serial.println("[OTA] Success — restarting.");
                ESP.restart();
                return;
        }
    }

    Serial.println("[OTA] All attempts failed — staying on current firmware.");
    WiFi.disconnect(true);
}
