#pragma once
#include <stdint.h>

// Call once in setup() to configure LEDC and attach to PWM output pin.
void dimming_init();

// Sets LED PWM duty immediately (0–255).
void setBrightness(uint8_t value);

// Returns the currently applied PWM duty (0–255).
uint8_t getBrightness();

// Linearly fades from current brightness to target over `ms` milliseconds.
// Blocking — returns when the fade completes.
void fadeTo(uint8_t target, uint16_t ms);
