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

// Mirrors espnow_comm_set_schedule's mac semantics.
void espnow_comm_get_schedule(const uint8_t mac[6], uint32_t *interval_sec_out, time_t *anchor_epoch_out);

// Call this once, after espnow_comm_init(), to start the task that
// processes received sensor packets (drift calc + correction send, and
// answering provisioning requests).
void espnow_comm_start_processing_task(void);

// Non-blocking check: has a new sensor reading been processed since the
// last call? If so, returns true and fills in the details, so pc_comm_task
// can forward it to the PC app.
bool espnow_comm_get_latest_reading(uint8_t *mac_out, uint16_t *distance_mm_out);

// Non-blocking check: has a sensor announced itself for provisioning
// (and been sent an interval) since the last call? If so, returns true
// and fills in its MAC, so pc_comm_task can forward a PROVISIONING
// notice to the PC app -- purely informational/visibility, the sensor
// has already been answered by the time this fires.
bool espnow_comm_get_latest_provisioning_event(uint8_t *mac_out);

// How many sensors were registered via espnow_comm_init().
int espnow_comm_get_sensor_count(void);

// Fills in the MAC, current interval, and current anchor for sensor
// `index` (0..count-1, see espnow_comm_get_sensor_count()). Returns false
// if index is out of range. Used to announce every sensor's schedule to a
// newly-connected PC.
bool espnow_comm_get_sensor_info(int index, uint8_t mac_out[6], uint32_t *interval_sec_out, time_t *anchor_epoch_out);

#ifdef __cplusplus
}
#endif
