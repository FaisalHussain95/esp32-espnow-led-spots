#include <Arduino.h>
#include <WiFi.h>
#include "config.h"
#include "thermal.h"
#include "dimming.h"
#include "espnow_manager.h"
#include "provisioning.h"
#include "ota.h"

// ─── Global state ─────────────────────────────────────────────────────────────
uint8_t         SPOT_ID                = 0x01;  // Overwritten from NVS in setup()
static uint8_t  g_requested_brightness = 255;  // Master's intended brightness
static bool     g_is_on                = false;
static float    g_temperature          = 0.0f;
static uint8_t  g_thermal_state        = THERMAL_NORMAL;
static uint32_t g_last_thermal_ms      = 0;
static uint32_t g_last_report_ms       = 0;

// Pulse loop state
static bool     g_pulsing              = false;
static uint8_t  g_pulse_peak           = 100;
static uint16_t g_pulse_half_dur       = 250;  // half-cycle ms
static bool     g_pulse_rising         = true;
static uint32_t g_pulse_phase_start    = 0;

// ─── Forward declarations ─────────────────────────────────────────────────────
static void handleCommand(const esp_now_cmd_t &cmd);
static void runThermalPolicy();
static void updateStatusLED();

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(1500);  // Wait for USB CDC to reconnect after reset (C3 SuperMini)
    WiFi.mode(WIFI_STA);

    pinMode(PIN_STATUS_LED, OUTPUT);
    digitalWrite(PIN_STATUS_LED, STATUS_LED_OFF);

    // Write PMK + WiFi creds + spot ID to NVS on first boot
    provisioning_init();
    SPOT_ID = provisioning_get_spot_id();

    Serial.printf("[BOOT] Spot node ID=0x%02X  FW_VERSION=%d  MAC=%s\n",
                  SPOT_ID, FW_VERSION, WiFi.macAddress().c_str());

    analogReadResolution(12);       // 12-bit → 0–4095
    analogRead(PIN_NTC_ADC);        // Dummy read to initialise the ADC channel
    analogSetPinAttenuation(PIN_NTC_ADC, ADC_11db);  // Now set 11dB (0–3.3V range)

    dimming_init();
    espnow_init();  // Loads PMK, enables encryption, sends MSG_HELLO to master

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

    // 1b. Check for OTA trigger from master
    uint8_t ota_version = 0;
    if (espnow_getOtaTrigger(&ota_version)) {
        Serial.printf("[OTA] Triggered — target version %d\n", ota_version);
        ota_start(ota_version);  // Blocks until done or all retries fail
    }

    // 1c. Pulse loop — non-blocking linear ramp up/down
    if (g_pulsing) {
        uint32_t elapsed = now - g_pulse_phase_start;
        if (elapsed >= g_pulse_half_dur) {
            // Phase done — flip direction
            g_pulse_rising = !g_pulse_rising;
            g_pulse_phase_start = now;
            elapsed = 0;
        }
        uint8_t lo = PWM_MIN_FLOOR;
        uint8_t hi = g_pulse_peak;
        uint8_t val;
        if (g_pulse_rising) {
            val = lo + (uint8_t)((uint32_t)(hi - lo) * elapsed / g_pulse_half_dur);
        } else {
            val = hi - (uint8_t)((uint32_t)(hi - lo) * elapsed / g_pulse_half_dur);
        }
        setBrightness(val);
    }

    // 2. Thermal check every 1 second
    if (now - g_last_thermal_ms >= THERMAL_CHECK_INTERVAL_MS) {
        g_last_thermal_ms = now;
        runThermalPolicy();
    }

    // 3. Periodic status report to master every 10 seconds
    if (now - g_last_report_ms >= STATUS_REPORT_INTERVAL_MS) {
        g_last_report_ms = now;
        espnow_retryHelloIfNeeded();
        sendStatus(getBrightness(), g_temperature, g_thermal_state, g_is_on);
    }

    // 4. Safety: if master is unreachable, turn off LED
    if (espnow_masterUnreachable() && g_is_on) {
        Serial.println("[SAFE] Master unreachable — turning off LED");
        g_is_on = false;
        fadeTo(0, 500);
        thermalPID_reset();
    }

    // 5. Status LED heartbeat (non-uniform timing handled inside)
    updateStatusLED();
}

// ─── Command handler ──────────────────────────────────────────────────────────
static void handleCommand(const esp_now_cmd_t &cmd) {
    switch (cmd.command) {

        case CMD_SET_BRIGHTNESS:
            g_pulsing = false;
            g_requested_brightness = cmd.brightness;
            if (g_is_on) {
                uint8_t allowed = applyThermalThrottle(g_requested_brightness, g_temperature);
                setBrightness(allowed);
            }
            break;

        case CMD_TURN_ON:
            g_pulsing = false;
            g_is_on = true;
            if (cmd.brightness > 0) g_requested_brightness = cmd.brightness;
            {
                uint8_t allowed = applyThermalThrottle(g_requested_brightness, g_temperature);
                fadeTo(allowed, 300);  // 300ms fade-in
            }
            break;

        case CMD_TURN_OFF:
            g_pulsing = false;
            g_is_on = false;
            fadeTo(0, 500);  // 500ms fade-out
            thermalPID_reset();
            break;

        case CMD_PULSE: {
            // Start looping pulse — runs in main loop, any other cmd stops it
            g_pulse_peak     = cmd.brightness > 0 ? cmd.brightness : 100;
            uint16_t dur     = cmd.param > 0 ? cmd.param : 500;
            g_pulse_half_dur = dur / 2;
            g_pulsing        = true;
            g_pulse_rising   = true;
            g_pulse_phase_start = millis();
            g_is_on          = true;
            setBrightness(PWM_MIN_FLOOR);
            break;
        }

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
// NORMAL   → 1 s on / 2 s off
// THROTTLE → solid ON
// CRITICAL → rapid 100 ms on / 100 ms off
static void updateStatusLED() {
    static bool     led_on        = false;
    static uint32_t phase_end_ms  = 0;

    uint32_t now = millis();
    if (now < phase_end_ms) return;  // Still in current phase

    if (g_thermal_state == THERMAL_THROTTLE) {
        digitalWrite(PIN_STATUS_LED, STATUS_LED_ON);
        led_on       = true;
        phase_end_ms = now + STATUS_LED_ON_MS;  // Re-check every 1 s (solid on)
        return;
    }

    // Toggle and schedule next phase
    led_on = !led_on;
    digitalWrite(PIN_STATUS_LED, led_on ? STATUS_LED_ON : STATUS_LED_OFF);

    if (g_thermal_state == THERMAL_CRITICAL) {
        phase_end_ms = now + 100;               // Fast blink: 100 ms per phase
    } else {
        phase_end_ms = now + (led_on ? STATUS_LED_ON_MS : STATUS_LED_OFF_MS);
    }
}
