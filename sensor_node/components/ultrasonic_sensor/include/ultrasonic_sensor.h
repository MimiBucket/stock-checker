#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Configures the TRIG/ECHO GPIOs. Call once per wake cycle, before reading.
bool ultrasonic_sensor_init(void);

// Fires a ping and times the echo to produce a single distance reading.
bool ultrasonic_sensor_read(uint16_t *distance_mm_out);

// Releases the TRIG/ECHO GPIOs.
void ultrasonic_sensor_deinit(void);

#ifdef __cplusplus
}
#endif
