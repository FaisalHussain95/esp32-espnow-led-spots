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

uint8_t applyThermalThrottle(uint8_t requested, float tempC) {
    // Hard limits
    if (tempC >= TEMP_CRITICAL)     return PWM_MIN_FLOOR;
    if (tempC >= TEMP_THROTTLE_MAX) return PWM_MIN_FLOOR;

    // Below TEMP_NORMAL_MAX — full authority
    if (tempC <= TEMP_NORMAL_MAX) return requested;

    // Linear ramp-down between TEMP_NORMAL_MAX and TEMP_THROTTLE_MAX
    float ratio = (TEMP_THROTTLE_MAX - tempC) / (TEMP_THROTTLE_MAX - TEMP_NORMAL_MAX);
    float allowed = PWM_MIN_FLOOR + ratio * ((float)requested - PWM_MIN_FLOOR);
    if (allowed < PWM_MIN_FLOOR) allowed = PWM_MIN_FLOOR;
    if (allowed > (float)requested) allowed = (float)requested;
    return (uint8_t)allowed;
}
