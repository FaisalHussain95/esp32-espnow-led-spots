#include "espnow_manager.h"
#include "provisioning.h"
#include "config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <string.h>

// ─── Internal state ───────────────────────────────────────────────────────────
static volatile bool  _cmd_available = false;
static esp_now_cmd_t  _cmd_buffer;

static volatile bool  _ota_available       = false;
static uint8_t        _ota_target_version  = 0;

// Consecutive send failure counter — spot cuts LED if master is unreachable
#define SEND_FAIL_CUTOFF  5
static volatile uint8_t _send_fail_count = 0;

// HELLO ACK tracking — retry HELLO if master hasn't acknowledged
static volatile bool _hello_acked = false;

// ─── Receive callback (ISR context) ──────────────────────────────────────────
static void onDataReceive(const uint8_t *mac, const uint8_t *data, int len) {
    if (len < (int)sizeof(espnow_header_t)) return;

    espnow_header_t hdr;
    memcpy(&hdr, data, sizeof(espnow_header_t));

    _send_fail_count = 0;  // Any received packet means master is reachable
    _hello_acked = true;   // Master knows us if it's sending to us

    switch (hdr.msg_type) {

        case MSG_ACK:
            Serial.println("[ESPNOW] ACK — version OK");
            break;

        case MSG_WHOIS: {
            // Master rebooted and is re-discovering spots — re-send HELLO via broadcast
            uint8_t self_mac[6];
            WiFi.macAddress(self_mac);
            espnow_header_t hello = {};
            hello.msg_type   = MSG_HELLO;
            hello.fw_version = FW_VERSION;
            hello.attempt    = SPOT_ID;
            memcpy(hello.mac, self_mac, 6);
            static const uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
            esp_now_send(BCAST, (uint8_t *)&hello, sizeof(hello));
            Serial.println("[ESPNOW] WHOIS → re-sent HELLO (broadcast)");
            break;
        }

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
    if (status == ESP_NOW_SEND_SUCCESS) {
        _send_fail_count = 0;
    } else {
        if (_send_fail_count < 255) _send_fail_count++;
        Serial.printf("[ESPNOW] Send failed (%d/%d)\n", _send_fail_count, SEND_FAIL_CUTOFF);
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────
void espnow_init() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_max_tx_power(52);  // 13dBm — balance TX range vs 3.3V current spikes
    int8_t tx_power = 0;
    esp_wifi_get_max_tx_power(&tx_power);
    Serial.printf("[ESPNOW] WiFi channel: %d  TX power: %d (0.25dBm units)\n", WiFi.channel(), tx_power);

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

    // Register broadcast peer for HELLO (unencrypted) — works even if master
    // already has us registered as encrypted (unicast would be silently dropped).
    static const uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    esp_now_peer_info_t bcast_peer = {};
    memcpy(bcast_peer.peer_addr, BCAST, 6);
    bcast_peer.channel = 0;
    bcast_peer.encrypt = false;
    bcast_peer.ifidx   = WIFI_IF_STA;
    esp_now_add_peer(&bcast_peer);  // may already exist — OK

    // Send MSG_HELLO via broadcast so master always receives it
    _hello_acked = false;
    uint8_t self_mac[6];
    WiFi.macAddress(self_mac);

    espnow_header_t hello = {};
    hello.msg_type   = MSG_HELLO;
    hello.fw_version = FW_VERSION;
    hello.attempt    = SPOT_ID;  // Reuse attempt field to carry spot_id in HELLO
    memcpy(hello.mac, self_mac, 6);

    esp_err_t hello_err = esp_now_send(BCAST, (uint8_t *)&hello, sizeof(hello));
    Serial.printf("[ESPNOW] HELLO sent (broadcast) — FW_VERSION=%d SPOT_ID=0x%02X  err=0x%x\n",
                  FW_VERSION, SPOT_ID, hello_err);

    // Register master peer with encryption for all subsequent packets
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, MASTER_MAC, 6);
    peer.channel = 0;
    peer.encrypt = true;
    memcpy(peer.lmk, pmk, 16);
    peer.ifidx   = WIFI_IF_STA;
    esp_err_t add_err = esp_now_add_peer(&peer);
    Serial.printf("[ESPNOW] Master peer add: err=0x%x  encrypt=%d\n", add_err, peer.encrypt);
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

    esp_err_t err = esp_now_send(MASTER_MAC, (uint8_t *)&pkt, sizeof(pkt));
    if (err != ESP_OK) {
        Serial.printf("[ESPNOW] sendStatus err=0x%x\n", err);
    }
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

bool espnow_masterUnreachable() {
    return _send_fail_count >= SEND_FAIL_CUTOFF;
}

void espnow_retryHelloIfNeeded() {
    if (_hello_acked) return;

    Serial.println("[ESPNOW] No ACK yet — resending HELLO (broadcast)");
    uint8_t self_mac[6];
    WiFi.macAddress(self_mac);

    espnow_header_t hello = {};
    hello.msg_type   = MSG_HELLO;
    hello.fw_version = FW_VERSION;
    hello.attempt    = SPOT_ID;
    memcpy(hello.mac, self_mac, 6);

    static const uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    esp_now_send(BCAST, (uint8_t *)&hello, sizeof(hello));
}
