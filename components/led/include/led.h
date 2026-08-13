#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Configures the LED's GPIO pin as an output.
bool led_init(void);

// Turns the LED on if distance_mm indicates low/empty stock, off otherwise.
void led_update(uint16_t distance_mm);

// Latches the LED's current state so it survives deep sleep.
// Call this right before esp_deep_sleep_start().
void led_hold_for_sleep(void);

#ifdef __cplusplus
}
#endif