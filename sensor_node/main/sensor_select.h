#pragma once

// Comparing the ToF (VL53L1X) and ultrasonic (HC-SR04) sensors -- flip
// this to 1 and reflash to switch which one this build uses.
#define SENSOR_TYPE_ULTRASONIC 1

#if SENSOR_TYPE_ULTRASONIC
#include "ultrasonic_sensor.h"
#define distance_sensor_init ultrasonic_sensor_init
#define distance_sensor_read ultrasonic_sensor_read
#define distance_sensor_deinit ultrasonic_sensor_deinit
#else
#include "tof_sensor.h"
#define distance_sensor_init tof_sensor_init
#define distance_sensor_read tof_sensor_read
#define distance_sensor_deinit tof_sensor_deinit
#endif
