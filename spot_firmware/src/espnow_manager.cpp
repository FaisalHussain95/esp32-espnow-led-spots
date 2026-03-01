#include "espnow_manager.h"
#include "config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <string.h>

// ─── Internal state ───────────────────────────────────────────────────────────
// _cmd_available is volatile: written by the ESP-NOW callback (core 0),
// read by loop() (core 1). Flag is set only after the memcpy completes.
static volatile bool  _cmd_available = false;
static esp_now_cmd_t  _cmd_buffer;

// ─── Receive callback (runs on ESP-NOW/WiFi task, core 0) ────────────────────
static void onDataReceive(const uint8_t *mac, const uint8_t *data, int len) {
    if (len != sizeof(esp_now_cmd_t)) return;

    esp_now_cmd_t pkt;
    memcpy(&pkt, data, sizeof(esp_now_cmd_t));

    // Ignore packets not addressed to this spot or broadcast
    if (pkt.spot_id != SPOT_ID && pkt.spot_id != 0xFF) return;

    memcpy(&_cmd_buffer, &pkt, sizeof(esp_now_cmd_t));
    _cmd_available = true;
}

// ─── Send callback (optional — used for debug) ───────────────────────────────
static void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS) {
        Serial.println("[ESP-NOW] Send failed");
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────
void espnow_init() {
    // ESP-NOW requires WiFi in station mode but does not connect to any AP
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    if (esp_now_init() != ESP_OK) {
        Serial.println("[ERROR] ESP-NOW init failed");
        return;
    }

    esp_now_register_recv_cb(onDataReceive);
    esp_now_register_send_cb(onDataSent);

    // Register master as a peer
    // channel=0: use the current WiFi channel (defaults to 1 when not connected)
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, MASTER_MAC, 6);
    peer.channel = 0;
    peer.encrypt = false;
    peer.ifidx   = WIFI_IF_STA;

    if (esp_now_add_peer(&peer) != ESP_OK) {
        Serial.println("[ERROR] Failed to add master peer");
    }
}

void sendStatus(uint8_t brightness, float temperature,
                uint8_t thermal_state, bool is_on) {
    esp_now_status_t pkt;
    pkt.spot_id      = SPOT_ID;
    pkt.brightness   = brightness;
    pkt.temperature  = temperature;
    pkt.thermal_state = thermal_state;
    pkt.is_on        = is_on;

    esp_now_send(MASTER_MAC, (uint8_t *)&pkt, sizeof(esp_now_status_t));
}

bool espnow_getCommand(esp_now_cmd_t *cmd) {
    if (!_cmd_available) return false;
    memcpy(cmd, &_cmd_buffer, sizeof(esp_now_cmd_t));
    _cmd_available = false;
    return true;
}
