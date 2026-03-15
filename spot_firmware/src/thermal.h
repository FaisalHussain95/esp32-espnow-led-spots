#pragma once
#include <stdint.h>

// Reads NTC with 32x oversampling and Steinhart-Hart conversion.
// Returns temperature in degrees Celsius.
// Returns -999.0f if ADC is saturated (open/short NTC circuit).
float readTemperatureC();

// Returns THERMAL_NORMAL, THERMAL_THROTTLE, or THERMAL_CRITICAL.
uint8_t getThermalState(float tempC);

// Linear throttle: returns the thermally-allowed PWM for a given requested brightness.
// Full authority below TEMP_NORMAL_MAX, linear ramp to PWM_MIN_FLOOR at TEMP_THROTTLE_MAX.
uint8_t applyThermalThrottle(uint8_t requested, float tempC);
