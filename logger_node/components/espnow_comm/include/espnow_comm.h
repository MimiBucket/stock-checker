#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// sensor_macs: array of MAC addresses (each 6 bytes) to register as peers.
// count: how many entries are in sensor_macs.
bool espnow_comm_init(const uint8_t sensor_macs[][6], int count);

// Sets the reporting schedule a sensor should use: interval_sec apart,
// aligned to anchor_epoch (a Unix timestamp) -- i.e. every valid wake
// time is anchor_epoch + k*interval_sec for some integer k, so two
// sensors given the same interval and anchor always wake in lockstep
// regardless of when either of them happened to last check in.
// Pass anchor_epoch = 0 for plain wall-clock alignment (e.g. every hour
// on the hour) with no specific reference sensor/event.
// mac == NULL means "the default applied to every registered sensor"
// (used for a PC SETFREQ ALL) and also updates every sensor's current
// schedule immediately, so the change is visible without waiting for
// each one to individually report back in.
// mac != NULL targets just that one registered sensor; unknown MACs are
// ignored.
void espnow_comm_set_schedule(const uint8_t mac[6], uint32_t interval_sec, time_t anchor_epoch);

typedef enum {
    ESPNOW_ADD_SENSOR_OK = 0,
    ESPNOW_ADD_SENSOR_ALREADY_REGISTERED,
    ESPNOW_ADD_SENSOR_TABLE_FULL,
    ESPNOW_ADD_SENSOR_PEER_FAILED,
} espnow_add_sensor_result_t;

// Registers a new sensor at runtime -- no rebuild/reflash of the logger
// needed. Adds it as an ESP-NOW peer, gives it the current default
// interval/anchor (same as any sensor that hasn't been individually
// SETFREQ'd), and persists it to NVS so it's still registered after the
// logger's next reboot without the PC having to re-add it.
espnow_add_sensor_result_t espnow_comm_add_sensor(const uint8_t mac[6]);

typedef enum {
    ESPNOW_REMOVE_SENSOR_OK = 0,
    ESPNOW_REMOVE_SENSOR_NOT_FOUND,
} espnow_remove_sensor_result_t;

// Unregisters a sensor at runtime -- e.g. for isolating whether a
// particular sensor's traffic is contributing to logger instability,
// without a rebuild/reflash. Also removes it as an ESP-NOW peer.
// If `mac` was registered via espnow_comm_add_sensor(), it's dropped from
// NVS too so it doesn't come back on the logger's next reboot. A mac from
// the compiled-in list passed to espnow_comm_init() isn't persisted
// there, so it reappears after the next reboot -- this call only removes
// it for the current session.
espnow_remove_sensor_result_t espnow_comm_remove_sensor(const uint8_t mac[6]);

// Mirrors espnow_comm_set_schedule's mac semantics.
void espnow_comm_get_schedule(const uint8_t mac[6], uint32_t *interval_sec_out, time_t *anchor_epoch_out);

// Call this once, after espnow_comm_init(), to start the task that
// processes received sensor packets (drift calc + correction send, and
// answering provisioning requests).
void espnow_comm_start_processing_task(void);

// Non-blocking pop of the oldest not-yet-delivered sensor reading (FIFO,
// backed by a real queue -- readings from multiple sensors arriving close
// together each get their own slot rather than clobbering one another).
// Returns false if none are pending. Call this in a loop, not just once
// per poll tick, if more than one reading might be waiting.
bool espnow_comm_get_latest_reading(uint8_t *mac_out, uint16_t *distance_mm_out);

// Non-blocking pop of the oldest not-yet-delivered provisioning event
// (same FIFO-queue backing as espnow_comm_get_latest_reading). Purely
// informational/visibility -- the sensor has already been answered by the
// time this fires. Call in a loop if more than one might be pending.
bool espnow_comm_get_latest_provisioning_event(uint8_t *mac_out);

// How many sensors were registered via espnow_comm_init().
int espnow_comm_get_sensor_count(void);

// How many raw packets are currently sitting in the rx queue, waiting for
// espnow_process_task to drain them. Debug/diagnostic use -- a
// consistently non-zero value means the processing task is falling behind.
int espnow_comm_get_rx_queue_depth(void);

// Fills in the MAC, current interval, and current anchor for sensor
// `index` (0..count-1, see espnow_comm_get_sensor_count()). Returns false
// if index is out of range. Used to announce every sensor's schedule to a
// newly-connected PC.
bool espnow_comm_get_sensor_info(int index, uint8_t mac_out[6], uint32_t *interval_sec_out, time_t *anchor_epoch_out);

#ifdef __cplusplus
}
#endif
