#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Configures the LED's GPIO pin as an output.
bool led_init(void);

// Turns the LED on/off. low_or_empty is the logger-computed status (from
// the PC-configured thresholds) -- the sensor itself doesn't know or
// store any threshold values.
void led_update(bool low_or_empty);

// Latches the LED's current state so it survives deep sleep.
// Call this right before esp_deep_sleep_start().
void led_hold_for_sleep(void);

#ifdef __cplusplus
}
#endif