#include <Arduino.h>
#include "config.h"
#include "thermal.h"
#include "dimming.h"
#include "espnow_manager.h"

// ─── Global state ─────────────────────────────────────────────────────────────
static uint8_t  g_requested_brightness = 255;  // Master's intended brightness
static bool     g_is_on                = false;
static float    g_temperature          = 0.0f;
static uint8_t  g_thermal_state        = THERMAL_NORMAL;
static uint32_t g_last_thermal_ms      = 0;
static uint32_t g_last_blink_ms        = 0;

// ─── Forward declarations ─────────────────────────────────────────────────────
static void handleCommand(const esp_now_cmd_t &cmd);
static void runThermalPolicy();
static void updateStatusLED();

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.printf("[BOOT] Spot node ID=0x%02X\n", SPOT_ID);

    pinMode(PIN_STATUS_LED, OUTPUT);
    digitalWrite(PIN_STATUS_LED, LOW);

    // Set ADC full-scale to 0–3.3V (default 0dB = 0–1.1V would saturate NTC reads)
    analogSetAttenuation(ADC_11db);
    analogReadResolution(12);  // 12-bit → 0–4095

    dimming_init();
    espnow_init();

    Serial.println("[BOOT] Ready.");
}

// ─── Main loop ────────────────────────────────────────────────────────────────
void loop() {
    uint32_t now = millis();

    // 1. Process incoming ESP-NOW commands
    esp_now_cmd_t cmd;
    if (espnow_getCommand(&cmd)) {
        handleCommand(cmd);
    }

    // 2. Thermal check every 1 second
    if (now - g_last_thermal_ms >= THERMAL_CHECK_INTERVAL_MS) {
        g_last_thermal_ms = now;
        runThermalPolicy();
    }

    // 3. Status LED heartbeat
    if (now - g_last_blink_ms >= STATUS_BLINK_INTERVAL_MS) {
        g_last_blink_ms = now;
        updateStatusLED();
    }
}

// ─── Command handler ──────────────────────────────────────────────────────────
static void handleCommand(const esp_now_cmd_t &cmd) {
    switch (cmd.command) {

        case CMD_SET_BRIGHTNESS:
            g_requested_brightness = cmd.brightness;
            if (g_is_on) {
                uint8_t allowed = applyThermalThrottle(g_requested_brightness, g_temperature);
                setBrightness(allowed);
            }
            break;

        case CMD_TURN_ON:
            g_is_on = true;
            if (cmd.brightness > 0) g_requested_brightness = cmd.brightness;
            {
                uint8_t allowed = applyThermalThrottle(g_requested_brightness, g_temperature);
                fadeTo(allowed, 300);  // 300ms fade-in
            }
            break;

        case CMD_TURN_OFF:
            g_is_on = false;
            fadeTo(0, 500);  // 500ms fade-out
            break;

        case CMD_REQUEST_STATUS:
            // No state change — just reply below
            break;

        default:
            Serial.printf("[WARN] Unknown command: 0x%02X\n", cmd.command);
            return;  // Don't send status for unknown commands
    }

    sendStatus(getBrightness(), g_temperature, g_thermal_state, g_is_on);
}

// ─── Thermal policy ───────────────────────────────────────────────────────────
static void runThermalPolicy() {
    float temp = readTemperatureC();

    // NTC fault (open or shorted cable) — cut output for safety
    if (temp < -100.0f) {
        Serial.println("[WARN] NTC fault — cutting to minimum");
        setBrightness(PWM_MIN_FLOOR);
        g_thermal_state = THERMAL_CRITICAL;
        sendStatus(getBrightness(), -999.0f, THERMAL_CRITICAL, g_is_on);
        return;
    }

    g_temperature = temp;
    uint8_t new_state = getThermalState(temp);

    // Re-apply throttle whenever the light is on
    if (g_is_on) {
        uint8_t allowed = applyThermalThrottle(g_requested_brightness, temp);
        setBrightness(allowed);
    }

    // Alert master on first entry into CRITICAL
    bool became_critical = (new_state == THERMAL_CRITICAL &&
                             g_thermal_state != THERMAL_CRITICAL);
    g_thermal_state = new_state;

    if (became_critical) {
        Serial.printf("[CRIT] Temp=%.1f°C — alerting master\n", temp);
        sendStatus(getBrightness(), temp, g_thermal_state, g_is_on);
    }

    Serial.printf("[THERM] %.1f°C  state=%d  pwm=%d\n",
                  temp, g_thermal_state, getBrightness());
}

// ─── Status LED ───────────────────────────────────────────────────────────────
static void updateStatusLED() {
    // NORMAL   → slow blink (toggled every STATUS_BLINK_INTERVAL_MS)
    // THROTTLE → solid ON
    // CRITICAL → same toggle rate as NORMAL (distinguishable by Serial output)
    static bool led_state = false;

    if (g_thermal_state == THERMAL_THROTTLE) {
        digitalWrite(PIN_STATUS_LED, HIGH);
        led_state = true;
        return;
    }

    led_state = !led_state;
    digitalWrite(PIN_STATUS_LED, led_state ? HIGH : LOW);
}
