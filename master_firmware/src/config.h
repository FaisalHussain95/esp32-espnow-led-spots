#pragma once
#include <stdint.h>

// ─── UART2 Bridge (to WiFi ESP32-B) ──────────────────────────────────────────
// GPIO13 = TX → ESP32-B RX
// GPIO35 = RX ← ESP32-B TX  (input-only pin, safe at boot, no strapping role)
#define PIN_UART2_TX  13
#define PIN_UART2_RX  35

// Spots register dynamically on HELLO — no MAC table needed.
// spot_id range: 0x01–0xFE (0xFF reserved for broadcast)
#define NUM_SPOTS  254

// ESP-NOW broadcast address (sends to all peers in range)
static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ─── Firmware Version ─────────────────────────────────────────────────────────
#define FW_VERSION  26   // Master firmware version (for reference)

// ─── ESP-NOW Message Types ────────────────────────────────────────────────────
// Must match spot_firmware/src/config.h exactly.
#define MSG_HELLO       0x01
#define MSG_ACK         0x02
#define MSG_REJECT      0x03
#define MSG_OTA_NOW     0x04
#define MSG_OTA_FAILED  0x05
#define MSG_WHOIS       0x06  // master → all slaves: re-announce yourself (broadcast on master boot)
#define MSG_CMD         0x10
#define MSG_STATUS      0x11

// ─── ESP-NOW Command Bytes ────────────────────────────────────────────────────
#define CMD_SET_BRIGHTNESS  0x01
#define CMD_TURN_ON         0x02
#define CMD_TURN_OFF        0x03
#define CMD_REQUEST_STATUS  0x04
#define CMD_PULSE           0x05

// ─── Thermal States (for display) ────────────────────────────────────────────
#define THERMAL_NORMAL    0
#define THERMAL_THROTTLE  1
#define THERMAL_CRITICAL  2
#define STATE_PULSE       3

// ─── ESP-NOW Packet Structures ────────────────────────────────────────────────
// Must match spot_firmware/src/config.h exactly — same wire format.

typedef struct __attribute__((packed)) {
    uint8_t msg_type;
    uint8_t fw_version;
    uint8_t attempt;
    uint8_t mac[6];
} espnow_header_t;

typedef struct __attribute__((packed)) {
    uint8_t  spot_id;
    uint8_t  brightness;
    uint8_t  command;
    uint16_t param;       // CMD_PULSE: duration in ms (0 = default 500ms)
} esp_now_cmd_t;

typedef struct __attribute__((packed)) {
    uint8_t spot_id;
    uint8_t brightness;
    float   temperature;
    uint8_t thermal_state;
    bool    is_on;
} esp_now_status_t;

typedef struct __attribute__((packed)) {
    espnow_header_t header;
    esp_now_cmd_t   cmd;
} espnow_cmd_packet_t;

typedef struct __attribute__((packed)) {
    espnow_header_t  header;
    esp_now_status_t status;
} espnow_status_packet_t;

typedef struct __attribute__((packed)) {
    espnow_header_t header;
    uint8_t         target_version;
} espnow_ota_packet_t;
