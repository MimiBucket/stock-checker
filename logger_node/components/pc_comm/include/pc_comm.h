#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// Sets up UART. Call once at startup.
void pc_comm_init(void);

// Blocking: waits for the PC's initial SETTIME command. Call this from
// app_main before starting anything else that depends on real time.
void pc_comm_wait_for_initial_sync(void);

// Starts the ongoing task that handles periodic time resync and forwards
// processed sensor readings to the PC. Call after espnow_comm is ready.
void pc_comm_start_task(void);

// Formats and sends "FREQ <mac> <interval_sec> <anchor_epoch>" to the PC
// for one sensor. Call whenever that sensor's schedule changes (e.g.
// right after handling a targeted SETFREQ command).
void pc_comm_send_freq(const uint8_t mac[6], uint32_t interval_sec, time_t anchor_epoch);

// Sends a "FREQ <mac> <interval_sec> <anchor_epoch>" line for every
// currently registered sensor. Call once after time sync completes (so a
// newly-connected PC gets the full picture), and again after a SETFREQ
// ALL command.
void pc_comm_send_all_freq(void);

// Formats and sends "SENSORS <mac1,mac2,...>" to the PC. Call once, right
// after espnow_comm_init() completes.
void pc_comm_send_sensor_list(const uint8_t sensor_macs[][6], int count);

// Formats and sends "PROVISIONING <mac>" to the PC -- purely informational,
// telling it a sensor is currently negotiating its interval with the
// logger (already-answered by the time this is sent).
void pc_comm_send_provisioning(const uint8_t mac[6]);

#ifdef __cplusplus
}
#endif