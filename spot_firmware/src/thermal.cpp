#include "thermal.h"
#include "config.h"
#include <Arduino.h>
#include <math.h>

float readTemperatureC() {
    // 32x oversampling to reduce ADC noise
    uint32_t adc_sum = 0;
    for (int i = 0; i < ADC_SAMPLES; i++) {
        adc_sum += analogRead(PIN_NTC_ADC);
    }
    float adc_avg = (float)adc_sum / ADC_SAMPLES;

    // Guard against open circuit (ADC near 3.3V) or short circuit (ADC near 0V)
    if (adc_avg < 10.0f || adc_avg > (ADC_MAX_VALUE - 10.0f)) {
        return -999.0f;
    }

    // Voltage divider: 3.3V → [NTC 10kΩ] → ADC node → [R_fixed=5.1kΩ] → GND
    //                                               → [100nF]          → GND
    // V_adc = 3.3V * R_fixed / (R_ntc + R_fixed)
    // Solving for R_ntc: R_ntc = R_fixed * (ADC_MAX - adc_avg) / adc_avg
    float r_ntc = NTC_R_PULLUP * (ADC_MAX_VALUE - adc_avg) / adc_avg;

    // Steinhart-Hart simplified Beta equation:
    //   1/T = 1/T0 + (1/B) * ln(R/R0)
    // logf() is the natural logarithm (ln), not log10.
    float ln_ratio = logf(r_ntc / NTC_R0);
    float t_kelvin = 1.0f / ((1.0f / NTC_T0) + (ln_ratio / NTC_B_COEFF));

    return t_kelvin - 273.15f;
}

uint8_t getThermalState(float tempC) {
    if (tempC >= TEMP_CRITICAL)   return THERMAL_CRITICAL;
    if (tempC >= TEMP_NORMAL_MAX) return THERMAL_THROTTLE;
    return THERMAL_NORMAL;
}

// ─── PID controller state ─────────────────────────────────────────────────────
static float _pid_integral  = 0.0f;
static float _prev_temp     = 0.0f;
static bool  _prev_temp_valid = false;

void thermalPID_reset() {
    _pid_integral    = 0.0f;
    _prev_temp_valid = false;
}

uint8_t applyThermalThrottle(uint8_t requested, float tempC) {
    // Hard limits take priority
    if (tempC >= TEMP_CRITICAL)     return PWM_MIN_FLOOR;
    if (tempC >= TEMP_THROTTLE_MAX) return PWM_MIN_FLOOR;

    // Rate of change (°C/s) — positive means rising
    float dT = 0.0f;
    if (_prev_temp_valid) {
        dT = tempC - _prev_temp;  // dt = 1s implicit
    }
    _prev_temp       = tempC;
    _prev_temp_valid = true;

    float error = tempC - TEMP_PID_TARGET;  // positive = too hot

    // Below target and cooling or stable — let integral decay slowly
    if (error <= 0.0f && dT <= 0.0f) {
        _pid_integral *= 0.98f;
        float correction = THERMAL_PID_KI * _pid_integral;
        float allowed = (float)requested - correction;
        if (allowed < PWM_MIN_FLOOR) allowed = PWM_MIN_FLOOR;
        if (allowed > (float)requested) allowed = (float)requested;
        return (uint8_t)allowed;
    }

    // Below target but rising — apply D term only to slow the climb early
    if (error <= 0.0f && dT > 0.0f) {
        float correction = THERMAL_PID_KI * _pid_integral + THERMAL_PID_KD * dT;
        float allowed = (float)requested - correction;
        if (allowed < PWM_MIN_FLOOR) allowed = PWM_MIN_FLOOR;
        if (allowed > (float)requested) allowed = (float)requested;
        Serial.printf("[PID] pre-act dT=%.2f  I=%.1f  corr=%.0f  pwm=%.0f\n",
                      dT, _pid_integral, correction, allowed);
        return (uint8_t)allowed;
    }

    // Above target — full PID
    float correction = THERMAL_PID_KP * error
                     + THERMAL_PID_KI * _pid_integral
                     + THERMAL_PID_KD * dT;

    float allowed = (float)requested - correction;
    if (allowed < PWM_MIN_FLOOR) allowed = PWM_MIN_FLOOR;
    if (allowed > (float)requested) allowed = (float)requested;

    // Anti-windup: only integrate when not saturated
    bool saturated = (allowed <= PWM_MIN_FLOOR) || (allowed >= (float)requested);
    if (!saturated) {
        _pid_integral += error;
    }

    Serial.printf("[PID] err=%.1f  dT=%.2f  I=%.1f  corr=%.0f  pwm=%.0f\n",
                  error, dT, _pid_integral, correction, allowed);

    return (uint8_t)allowed;
}
