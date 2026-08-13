#pragma once

#include <stdint.h> 
#include <stdbool.h> 

#ifdef __cplusplus
extern "C" {
#endif

// Initializes I2C bus + sensor. Call once per wake cycle, before reading.
bool tof_sensor_init(void);

// Takes a single distance reading from the sensor.
bool tof_sensor_read(uint16_t *distance_mm_out);

// Tears down the I2C bus and frees the sensor object.
void tof_sensor_deinit(void);

#ifdef __cplusplus
} 
#endif

