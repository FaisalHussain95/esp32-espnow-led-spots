#include "dimming.h"
#include "config.h"
#include <Arduino.h>

static uint8_t _current_brightness = 0;

void dimming_init() {
    ledcSetup(LEDC_CHANNEL, LEDC_FREQ_HZ, LEDC_RESOLUTION);
    ledcAttachPin(PIN_PWM_OUT, LEDC_CHANNEL);
    ledcWrite(LEDC_CHANNEL, 0);
    _current_brightness = 0;
}

void setBrightness(uint8_t value) {
    if (value > PWM_MAX) value = PWM_MAX;
    _current_brightness = value;
    ledcWrite(LEDC_CHANNEL, value);
}

uint8_t getBrightness() {
    return _current_brightness;
}

void fadeTo(uint8_t target, uint16_t ms) {
    uint8_t start = _current_brightness;
    if (start == target) return;

    int steps = ms / FADE_STEP_DELAY_MS;
    if (steps < 1) steps = 1;

    for (int i = 1; i <= steps; i++) {
        int value = (int)start + ((int)target - (int)start) * i / steps;
        setBrightness((uint8_t)value);
        delay(FADE_STEP_DELAY_MS);
    }
    setBrightness(target);  // Guarantee exact landing value
}
