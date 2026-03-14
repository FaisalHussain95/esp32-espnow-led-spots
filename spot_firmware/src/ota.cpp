#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <lwip/dns.h>
#include <lwip/ip_addr.h>
#include <Preferences.h>
#include "ota.h"
#include "config.h"
#include "espnow_manager.h"

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
        }

        if (WiFi.status() != WL_CONNECTED) {
            Serial.printf("[OTA] WiFi connect failed — SSID='%s' password='%s'\n",
                          ssid.c_str(), password.c_str());
            if (attempt < OTA_MAX_ATTEMPTS) {
                Serial.printf("[OTA] Retrying in %ds...\n", OTA_RETRY_DELAY_MS / 1000);
                delay(OTA_RETRY_DELAY_MS);
            }
            continue;
        }

        // Force Cloudflare DNS — both Arduino layer and lwIP layer
        WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(),
                    IPAddress(1, 1, 1, 1), IPAddress(1, 0, 0, 1));
        ip_addr_t dns1, dns2;
        ipaddr_aton("1.1.1.1", &dns1);
        ipaddr_aton("1.0.0.1", &dns2);
        dns_setserver(0, &dns1);
        dns_setserver(1, &dns2);
        delay(500);

        // Verify DNS works before attempting OTA
        IPAddress resolved;
        if (!WiFi.hostByName("github.com", resolved)) {
            Serial.println("[OTA] DNS resolve github.com failed — retrying");
            if (attempt < OTA_MAX_ATTEMPTS) {
                WiFi.disconnect(true);
                delay(OTA_RETRY_DELAY_MS);
            }
            continue;
        }

        Serial.printf("[OTA] WiFi OK. IP: %s  DNS OK: github.com=%s  free heap: %u\n",
                      WiFi.localIP().toString().c_str(),
                      resolved.toString().c_str(), ESP.getFreeHeap());

        WiFiClientSecure client;
        client.setInsecure();
        client.setHandshakeTimeout(30);
        t_httpUpdate_return result = httpUpdate.update(client, url);

        switch (result) {
            case HTTP_UPDATE_OK:
                Serial.println("[OTA] Success — restarting.");
                ESP.restart();
                return;
            case HTTP_UPDATE_NO_UPDATES:
                Serial.println("[OTA] Server says no update available.");
                WiFi.disconnect(true);
                espnow_init();
                return;
            case HTTP_UPDATE_FAILED:
                Serial.printf("[OTA] Attempt %d failed: %s\n",
                              attempt, httpUpdate.getLastErrorString().c_str());
                if (attempt < OTA_MAX_ATTEMPTS) {
                    Serial.printf("[OTA] Retrying in %ds...\n", OTA_RETRY_DELAY_MS / 1000);
                    delay(OTA_RETRY_DELAY_MS);
                }
                break;
        }
    }

    Serial.println("[OTA] All attempts failed — staying on current firmware.");
    WiFi.disconnect(true);
    espnow_init();
    sendStatus(0, -1.0f, THERMAL_NORMAL, false, MSG_OTA_FAILED, OTA_MAX_ATTEMPTS);
}
