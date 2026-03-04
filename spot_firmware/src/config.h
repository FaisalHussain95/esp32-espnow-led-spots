#pragma once
#include <stdint.h>

// ─── Identity ─────────────────────────────────────────────────────────────────
// Change SPOT_ID (0x01–0x0A) and MASTER_MAC for each unit before flashing.
#define SPOT_ID  0x01

// Placeholder: replace with actual master MAC (run WiFi.macAddress() on master)
static const uint8_t MASTER_MAC[6] = {0x24, 0x6F, 0x28, 0xB1, 0xC3, 0x2C};

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
#define TEMP_THROTTLE_MAX 75.0f    // 60–75°C: linear brightness reduction
#define TEMP_FLOOR_MAX    85.0f    // 75–85°C: hold at PWM_MIN_FLOOR
#define TEMP_CRITICAL     85.0f    // Above this: CRITICAL — alert master

// ─── Dimming Limits ───────────────────────────────────────────────────────────
#define PWM_MIN_FLOOR   20         // Minimum PWM in throttle/critical states
#define PWM_MAX        255

// ─── Timing (ms) ──────────────────────────────────────────────────────────────
#define THERMAL_CHECK_INTERVAL_MS  1000
#define STATUS_REPORT_INTERVAL_MS 10000  // Periodic unsolicited status to master
#define FADE_STEP_DELAY_MS           10
#define STATUS_LED_ON_MS           1000   // NORMAL state: LED on duration
#define STATUS_LED_OFF_MS          2000   // NORMAL state: LED off duration

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
