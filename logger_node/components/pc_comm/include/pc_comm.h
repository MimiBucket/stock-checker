#pragma once

#include <stdbool.h>

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

#ifdef __cplusplus
}
#endif