#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Preferences.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include "ota.h"
#include "config.h"
#include "espnow_manager.h"

static void notify_master_ota_failed(uint8_t attempt) {
    // ESP-NOW is deinited during OTA — notify after re-init at end of ota_start()
    (void)attempt;
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
        espnow_init();
        return;
    }

    // Build URL
    char url[192];
    snprintf(url, sizeof(url), OTA_BASE_URL, target_version, target_version);
    Serial.printf("[OTA] URL: %s\n", url);

    // Free heap for TLS by stopping ESP-NOW before WiFi
    esp_now_deinit();
    httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

    for (uint8_t attempt = 1; attempt <= OTA_MAX_ATTEMPTS; attempt++) {
        Serial.printf("[OTA] Attempt %d/%d...\n", attempt, OTA_MAX_ATTEMPTS);

        // Connect (or reconnect) to WiFi each attempt
        if (WiFi.status() != WL_CONNECTED) {
            Serial.printf("[OTA] Connecting to WiFi: %s", ssid.c_str());
            WiFi.begin(ssid.c_str(), password.c_str());
            uint32_t t0 = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
                delay(500);
                Serial.print(".");
            }
            Serial.println();
            if (WiFi.status() == WL_CONNECTED) {
                // Force Cloudflare DNS — router DNS can be unreliable for github.com
                WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(),
                            IPAddress(1, 1, 1, 1), IPAddress(1, 0, 0, 1));
            }
        }

        if (WiFi.status() != WL_CONNECTED) {
            Serial.printf("[OTA] WiFi connect failed — SSID='%s' password='%s'\n",
                          ssid.c_str(), password.c_str());
            notify_master_ota_failed(attempt);
            if (attempt < OTA_MAX_ATTEMPTS) {
                Serial.printf("[OTA] Retrying in %ds...\n", OTA_RETRY_DELAY_MS / 1000);
                delay(OTA_RETRY_DELAY_MS);
            }
            continue;
        }
        Serial.printf("[OTA] WiFi OK. IP: %s\n", WiFi.localIP().toString().c_str());

        WiFiClientSecure client;
        client.setInsecure();
        client.setHandshakeTimeout(30);
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
                WiFi.disconnect(true);
                espnow_init();
                return;

            case HTTP_UPDATE_OK:
                Serial.println("[OTA] Success — restarting.");
                ESP.restart();
                return;
        }
    }

    Serial.println("[OTA] All attempts failed — staying on current firmware.");
    WiFi.disconnect(true);
    espnow_init();
    sendStatus(0, -1.0f, THERMAL_NORMAL, false, MSG_OTA_FAILED, OTA_MAX_ATTEMPTS);
}
