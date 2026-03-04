#include "espnow_manager.h"
#include "provisioning.h"
#include "config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <string.h>

// ─── Internal state ───────────────────────────────────────────────────────────
static volatile bool  _cmd_available = false;
static esp_now_cmd_t  _cmd_buffer;

static volatile bool  _ota_available       = false;
static uint8_t        _ota_target_version  = 0;

// ─── Receive callback (ISR context) ──────────────────────────────────────────
static void onDataReceive(const uint8_t *mac, const uint8_t *data, int len) {
    if (len < (int)sizeof(espnow_header_t)) return;

    espnow_header_t hdr;
    memcpy(&hdr, data, sizeof(espnow_header_t));

    switch (hdr.msg_type) {

        case MSG_ACK:
            Serial.println("[ESPNOW] ACK — version OK");
            break;

        case MSG_REJECT:
        case MSG_OTA_NOW:
            if (len >= (int)sizeof(espnow_ota_packet_t)) {
                espnow_ota_packet_t pkt;
                memcpy(&pkt, data, sizeof(espnow_ota_packet_t));
                _ota_target_version = pkt.target_version;
                _ota_available = true;
            }
            break;

        case MSG_CMD:
            if (len >= (int)sizeof(espnow_cmd_packet_t)) {
                espnow_cmd_packet_t pkt;
                memcpy(&pkt, data, sizeof(espnow_cmd_packet_t));
                if (pkt.cmd.spot_id == SPOT_ID || pkt.cmd.spot_id == 0xFF) {
                    memcpy(&_cmd_buffer, &pkt.cmd, sizeof(esp_now_cmd_t));
                    _cmd_available = true;
                }
            }
            break;

        default:
            break;
    }
}

// ─── Send callback ────────────────────────────────────────────────────────────
static void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS) {
        Serial.println("[ESPNOW] Send failed");
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────
void espnow_init() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    if (esp_now_init() != ESP_OK) {
        Serial.println("[ERROR] ESP-NOW init failed");
        return;
    }

    // Load PMK and enable encryption
    uint8_t pmk[16];
    provisioning_get_pmk(pmk);
    if (esp_now_set_pmk(pmk) != ESP_OK) {
        Serial.println("[ERROR] ESP-NOW set PMK failed");
    }

    esp_now_register_recv_cb(onDataReceive);
    esp_now_register_send_cb(onDataSent);

    // Register master peer with encryption (PMK used as LMK — single shared key)
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, MASTER_MAC, 6);
    peer.channel = 0;
    peer.encrypt = true;
    memcpy(peer.lmk, pmk, 16);
    peer.ifidx   = WIFI_IF_STA;

    if (esp_now_add_peer(&peer) != ESP_OK) {
        Serial.println("[ERROR] Failed to add master peer");
    }

    // Send MSG_HELLO (master registers us dynamically and checks version)
    uint8_t self_mac[6];
    WiFi.macAddress(self_mac);

    espnow_header_t hello = {};
    hello.msg_type   = MSG_HELLO;
    hello.fw_version = FW_VERSION;
    hello.attempt    = 0;
    memcpy(hello.mac, self_mac, 6);

    esp_now_send(MASTER_MAC, (uint8_t *)&hello, sizeof(hello));
    Serial.printf("[ESPNOW] HELLO sent — FW_VERSION=%d\n", FW_VERSION);
}

void sendStatus(uint8_t brightness, float temperature,
                uint8_t thermal_state, bool is_on,
                uint8_t msg_type, uint8_t attempt) {
    uint8_t self_mac[6];
    WiFi.macAddress(self_mac);

    espnow_status_packet_t pkt = {};
    pkt.header.msg_type   = msg_type;
    pkt.header.fw_version = FW_VERSION;
    pkt.header.attempt    = attempt;
    memcpy(pkt.header.mac, self_mac, 6);

    pkt.status.spot_id       = SPOT_ID;
    pkt.status.brightness    = brightness;
    pkt.status.temperature   = temperature;
    pkt.status.thermal_state = thermal_state;
    pkt.status.is_on         = is_on;

    esp_now_send(MASTER_MAC, (uint8_t *)&pkt, sizeof(pkt));
}

bool espnow_getCommand(esp_now_cmd_t *cmd) {
    if (!_cmd_available) return false;
    memcpy(cmd, &_cmd_buffer, sizeof(esp_now_cmd_t));
    _cmd_available = false;
    return true;
}

bool espnow_getOtaTrigger(uint8_t *target_version) {
    if (!_ota_available) return false;
    *target_version = _ota_target_version;
    _ota_available  = false;
    return true;
}
