#pragma once
#include <stdint.h>

// ─── Identity ─────────────────────────────────────────────────────────────────
// Change SPOT_ID (0x01–0x0A) and MASTER_MAC for each unit before flashing.
#define SPOT_ID  0x01

// Placeholder: replace with actual master MAC (run WiFi.macAddress() on master)
static const uint8_t MASTER_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ─── GPIO Pin Assignments (ESP32 WROVER prototype) ────────────────────────────
#define PIN_PWM_OUT     18   // LEDC output → 100Ω → IRLZ44N gate
#define PIN_NTC_ADC     34   // ADC1_CH6, input-only pin — no internal pull
#define PIN_STATUS_LED   2   // Onboard LED, active HIGH

// ─── LEDC (PWM) Configuration ─────────────────────────────────────────────────
#define LEDC_CHANNEL     0
#define LEDC_TIMER       0
#define LEDC_FREQ_HZ  10000  // 10 kHz — inaudible, suitable for IRLZ44N gate
#define LEDC_RESOLUTION  8   // 8-bit → duty range 0–255

// ─── NTC / ADC Configuration ──────────────────────────────────────────────────
#define ADC_SAMPLES       32       // Oversampling count to reduce noise
#define NTC_B_COEFF     3950.0f    // Beta coefficient
#define NTC_R0         10000.0f    // NTC nominal resistance at T0 (ohms)
#define NTC_T0          298.15f    // T0 in Kelvin (25°C)
#define NTC_R_PULLUP   10000.0f    // Fixed pullup to 3.3V at ESP32 end (ohms)
#define ADC_MAX_VALUE   4095.0f    // 12-bit ADC full scale

// ─── Thermal Thresholds (°C) ──────────────────────────────────────────────────
#define TEMP_NORMAL_MAX   60.0f    // Below this: full PWM authority
#define TEMP_THROTTLE_MAX 75.0f    // 60–75°C: linear brightness reduction
#define TEMP_FLOOR_MAX    85.0f    // 75–85°C: hold at PWM_MIN_FLOOR
#define TEMP_CRITICAL     85.0f    // Above this: CRITICAL — alert master

// ─── Dimming Limits ───────────────────────────────────────────────────────────
#define PWM_MIN_FLOOR   20         // Minimum PWM in throttle/critical states
#define PWM_MAX        255

// ─── Timing (ms) ──────────────────────────────────────────────────────────────
#define THERMAL_CHECK_INTERVAL_MS  1000
#define FADE_STEP_DELAY_MS           10
#define STATUS_BLINK_INTERVAL_MS    500

// ─── ESP-NOW Command Bytes ────────────────────────────────────────────────────
#define CMD_SET_BRIGHTNESS  0x01
#define CMD_TURN_ON         0x02
#define CMD_TURN_OFF        0x03
#define CMD_REQUEST_STATUS  0x04

// ─── Thermal States ───────────────────────────────────────────────────────────
#define THERMAL_NORMAL    0
#define THERMAL_THROTTLE  1
#define THERMAL_CRITICAL  2

// ─── ESP-NOW Packet Structures ────────────────────────────────────────────────
// Packed to prevent compiler padding from misaligning bytes on the wire.

typedef struct __attribute__((packed)) {
    uint8_t spot_id;     // 0xFF = broadcast
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
