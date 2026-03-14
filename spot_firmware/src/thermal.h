#pragma once
#include <stdint.h>

// Reads NTC with 32x oversampling and Steinhart-Hart conversion.
// Returns temperature in degrees Celsius.
// Returns -999.0f if ADC is saturated (open/short NTC circuit).
float readTemperatureC();

// Returns THERMAL_NORMAL, THERMAL_THROTTLE, or THERMAL_CRITICAL.
uint8_t getThermalState(float tempC);

// PI controller: returns the thermally-allowed PWM cap for a given requested brightness.
// Call every thermal check cycle (1s). Below TEMP_PID_TARGET the PI is inactive.
// Call thermalPID_reset() when the light is turned off to clear the integrator.
uint8_t applyThermalThrottle(uint8_t requested, float tempC);
void    thermalPID_reset();
