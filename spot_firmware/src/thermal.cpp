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

    // Voltage divider: 3.3V → [R_pullup=10kΩ] → node → [NTC] → GND
    // ADC reads at node. V_node = 3.3V * R_ntc / (R_pullup + R_ntc)
    // Solving for R_ntc: R_ntc = R_pullup * adc_avg / (ADC_MAX - adc_avg)
    float r_ntc = NTC_R_PULLUP * adc_avg / (ADC_MAX_VALUE - adc_avg);

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

uint8_t applyThermalThrottle(uint8_t requested, float tempC) {
    if (tempC < TEMP_NORMAL_MAX) {
        // NORMAL: full authority
        return requested;
    }

    if (tempC >= TEMP_CRITICAL) {
        // CRITICAL: hard floor
        return PWM_MIN_FLOOR;
    }

    if (tempC >= TEMP_THROTTLE_MAX) {
        // 75–85°C: floor enforced
        return (requested < PWM_MIN_FLOOR) ? requested : PWM_MIN_FLOOR;
    }

    // 60–75°C: linear reduction
    // scale: 1.0 at 60°C → 0.0 at 75°C
    // allowed: PWM_MIN_FLOOR + scale * (requested - PWM_MIN_FLOOR)
    float scale = 1.0f - ((tempC - TEMP_NORMAL_MAX) / (TEMP_THROTTLE_MAX - TEMP_NORMAL_MAX));
    float allowed = PWM_MIN_FLOOR + scale * ((float)requested - PWM_MIN_FLOOR);
    uint8_t result = (uint8_t)allowed;
    return (result < PWM_MIN_FLOOR) ? PWM_MIN_FLOOR : result;
}
