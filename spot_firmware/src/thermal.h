#pragma once
#include <stdint.h>

// Reads NTC with 32x oversampling and Steinhart-Hart conversion.
// Returns temperature in degrees Celsius.
// Returns -999.0f if ADC is saturated (open/short NTC circuit).
float readTemperatureC();

// Returns THERMAL_NORMAL, THERMAL_THROTTLE, or THERMAL_CRITICAL.
uint8_t getThermalState(float tempC);

// Returns the thermally-allowed PWM value for a given requested brightness.
// Call every thermal check cycle and re-apply the result to setBrightness().
uint8_t applyThermalThrottle(uint8_t requested, float tempC);
