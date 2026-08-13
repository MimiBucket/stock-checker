#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// sensor_macs: array of MAC addresses (each 6 bytes) to register as peers.
// count: how many entries are in sensor_macs.
bool espnow_comm_init(const uint8_t sensor_macs[][6], int count);

void espnow_comm_set_expected_interval(uint32_t interval_sec);

// Call this once, after espnow_comm_init(), to start the task that
// processes received sensor packets (drift calc + correction send).
void espnow_comm_start_processing_task(void);

// Non-blocking check: has a new sensor reading been processed since the
// last call? If so, returns true and fills in the details, so pc_comm_task
// can forward it to the PC app.
bool espnow_comm_get_latest_reading(uint8_t *mac_out, uint16_t *distance_mm_out);

#ifdef __cplusplus
}
#endif