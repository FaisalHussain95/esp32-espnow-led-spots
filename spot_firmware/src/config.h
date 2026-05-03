#pragma once
#include <stdint.h>

// ─── Identity ─────────────────────────────────────────────────────────────────
// SPOT_ID is set via CONFIG_SPOT_ID build flag in platformio.ini,
// written to NVS on first flash, and survives OTA updates.
// Use provisioning_get_spot_id() at runtime — do not use CONFIG_SPOT_ID directly.
extern uint8_t SPOT_ID;

// ─── GPIO Pin Assignments ─────────────────────────────────────────────────────
// ESP32 WROVER (prototype)
// #define PIN_PWM_OUT     18   // LEDC output → PT4115 DIM pin
// #define PIN_NTC_ADC     34   // ADC1_CH6, input-only pin — no internal pull
// #define PIN_STATUS_LED   2   // Onboard LED, active HIGH

// ESP32-C3 SuperMini (final)
#define PIN_PWM_OUT     10   // LEDC output → PT4115 DIM pin (no ADC, no strapping)
#define PIN_NTC_ADC      3   // ADC1_CH3 — safe with ESP-NOW/WiFi active (ADC1 only)
#define PIN_STATUS_LED   8   // Onboard blue LED, active LOW
#define STATUS_LED_ON   LOW  // GPIO8 onboard LED lights when driven LOW
#define STATUS_LED_OFF  HIGH

// ─── Fan Control ──────────────────────────────────────────────────────────────
#define PIN_FAN_PWM      4   // AO3400 gate — GPIO output drives fan MOSFET
#define PIN_FAN_EN       5   // MT3608 EN — drive HIGH to enable 5V boost rail

#define FAN_ON_TEMP     45.0f  // Turn fan on above this (°C)
#define FAN_OFF_TEMP    40.0f  // Turn fan off below this (°C) — hysteresis
#define FAN_BOOST_DELAY_MS  2  // Wait after enabling MT3608 before driving MOSFET

// ─── LEDC (PWM) Configuration ─────────────────────────────────────────────────
#define LEDC_CHANNEL     0
#define LEDC_TIMER       0
#define LEDC_FREQ_HZ  10000  // 10 kHz — inaudible, within PT4115 DIM pin range
#define LEDC_RESOLUTION  8   // 8-bit → duty range 0–255

// ─── NTC / ADC Configuration ──────────────────────────────────────────────────
#define ADC_SAMPLES       32       // Oversampling count to reduce noise
#define NTC_B_COEFF     3950.0f    // Beta coefficient
#define NTC_R0         10000.0f    // NTC nominal resistance at T0 (ohms)
#define NTC_T0          298.15f    // T0 in Kelvin (25°C)
#define NTC_R_PULLUP    5100.0f    // Fixed resistor to GND (5.1kΩ); NTC is on the 3.3V side
#define ADC_MAX_VALUE   4095.0f    // 12-bit ADC full scale

// ─── Thermal Thresholds (°C) ──────────────────────────────────────────────────
#define TEMP_NORMAL_MAX   60.0f    // Below this: full PWM authority
#define TEMP_THROTTLE_MAX 75.0f    // Above this: hard floor enforced
#define TEMP_CRITICAL     85.0f    // Above this: CRITICAL — alert master

// ─── Dimming Limits ───────────────────────────────────────────────────────────
#define PWM_MIN_FLOOR   20         // Minimum PWM in throttle/critical states
#define PWM_MAX        160         // ~5W operating point for COB LED

// ─── Timing (ms) ──────────────────────────────────────────────────────────────
#define THERMAL_CHECK_INTERVAL_MS  1000
#define STATUS_REPORT_INTERVAL_MS 10000  // Periodic unsolicited status to master
#define FADE_STEP_DELAY_MS           10
#define STATUS_LED_ON_MS           1000   // NORMAL state: LED on duration
#define STATUS_LED_OFF_MS          2000   // NORMAL state: LED off duration

// ─── ESP-NOW Radio ────────────────────────────────────────────────────────────
#define ESPNOW_CHANNEL  11   // WiFi channel for all ESP-NOW traffic (1, 6, or 11)

// ─── Firmware Version & OTA ───────────────────────────────────────────────────
#define FW_VERSION  31
#define OTA_MAX_ATTEMPTS  5
#define OTA_RETRY_DELAY_MS  10000
#define OTA_BASE_URL \
    "https://github.com/FaisalHussain95/esp32-espnow-led-spots/releases/download/v%d/slave_c3_supermini_v%d.bin"

// ─── ESP-NOW Message Types ────────────────────────────────────────────────────
#define MSG_HELLO       0x01  // slave → master: announce version (unencrypted OK)
#define MSG_ACK         0x02  // master → slave: version ok
#define MSG_REJECT      0x03  // master → slave: version outdated, OTA incoming
#define MSG_OTA_NOW     0x04  // master → all slaves: trigger OTA to target_version
#define MSG_OTA_FAILED  0x05  // slave → master: OTA attempt failed
#define MSG_WHOIS       0x06  // master → all slaves: re-announce yourself (broadcast on master boot)
#define MSG_CMD         0x10  // master → slave: LED command (wraps esp_now_cmd_t)
#define MSG_STATUS      0x11  // slave → master: status reply (wraps esp_now_status_t)

// ─── ESP-NOW Command Bytes (CMD_* used inside MSG_CMD payload) ────────────────
#define CMD_SET_BRIGHTNESS  0x01
#define CMD_TURN_ON         0x02
#define CMD_TURN_OFF        0x03
#define CMD_REQUEST_STATUS  0x04
#define CMD_PULSE           0x05

// ─── Thermal States ───────────────────────────────────────────────────────────
#define THERMAL_NORMAL    0
#define THERMAL_THROTTLE  1
#define THERMAL_CRITICAL  2
#define STATE_PULSE       3

// ─── ESP-NOW Packet Structures ────────────────────────────────────────────────
// Packed to prevent compiler padding from misaligning bytes on the wire.

// Envelope header — all messages start with this (9 bytes)
typedef struct __attribute__((packed)) {
    uint8_t msg_type;    // MSG_* above
    uint8_t fw_version;  // sender's firmware version
    uint8_t attempt;     // OTA retry count (0 for non-OTA messages)
    uint8_t mac[6];      // sender MAC address
} espnow_header_t;

// Master → Slave LED command payload (inside MSG_CMD)
typedef struct __attribute__((packed)) {
    uint8_t  spot_id;     // 0xFF = broadcast
    uint8_t  brightness;  // 0–255
    uint8_t  command;     // CMD_* values above
    uint16_t param;       // CMD_PULSE: duration in ms (0 = default 500ms)
} esp_now_cmd_t;

// Slave → Master status payload (inside MSG_STATUS)
typedef struct __attribute__((packed)) {
    uint8_t spot_id;
    uint8_t brightness;
    float   temperature;
    uint8_t thermal_state;  // THERMAL_* values above
    bool    is_on;
} esp_now_status_t;

// Full MSG_CMD packet (master → slave)
typedef struct __attribute__((packed)) {
    espnow_header_t header;
    esp_now_cmd_t   cmd;
} espnow_cmd_packet_t;

// Full MSG_STATUS packet (slave → master)
typedef struct __attribute__((packed)) {
    espnow_header_t  header;
    esp_now_status_t status;
} espnow_status_packet_t;

// Full MSG_OTA_NOW packet (master → all slaves)
typedef struct __attribute__((packed)) {
    espnow_header_t header;
    uint8_t         target_version;
} espnow_ota_packet_t;
