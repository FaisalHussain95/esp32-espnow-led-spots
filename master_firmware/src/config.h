#pragma once
#include <stdint.h>

// ─── Spot MAC Table ───────────────────────────────────────────────────────────
// Index 0 = spot 0x01, index 1 = spot 0x02, ... index 9 = spot 0x0A
// Fill in each spot's MAC after reading it from the spot's boot log.
// Run: pio device monitor on the spot, look for "MAC: xx:xx:xx:xx:xx:xx"
static const uint8_t SPOT_MACS[10][6] = {
    {0x70, 0x4B, 0xCA, 0x81, 0xA3, 0xAC},  // Spot 0x01 — WROVER prototype
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},  // Spot 0x02 — not yet assigned
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},  // Spot 0x03
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},  // Spot 0x04
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},  // Spot 0x05
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},  // Spot 0x06
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},  // Spot 0x07
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},  // Spot 0x08
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},  // Spot 0x09
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},  // Spot 0x0A
};
#define NUM_SPOTS  10

// ESP-NOW broadcast address (sends to all peers in range)
static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ─── ESP-NOW Command Bytes ────────────────────────────────────────────────────
#define CMD_SET_BRIGHTNESS  0x01
#define CMD_TURN_ON         0x02
#define CMD_TURN_OFF        0x03
#define CMD_REQUEST_STATUS  0x04

// ─── Thermal States (for display) ────────────────────────────────────────────
#define THERMAL_NORMAL    0
#define THERMAL_THROTTLE  1
#define THERMAL_CRITICAL  2

// ─── ESP-NOW Packet Structures ────────────────────────────────────────────────
// Must match spot_firmware/src/config.h exactly — same wire format.

typedef struct __attribute__((packed)) {
    uint8_t spot_id;     // 0xFF = broadcast all
    uint8_t brightness;  // 0–255
    uint8_t command;     // CMD_* values above
} esp_now_cmd_t;

typedef struct __attribute__((packed)) {
    uint8_t spot_id;
    uint8_t brightness;
    float   temperature;
    uint8_t thermal_state;  // THERMAL_* values above
    bool    is_on;
} esp_now_status_t;
