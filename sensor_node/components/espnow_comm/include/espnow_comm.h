#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initializes WiFi (station mode, required for ESP-NOW) + ESP-NOW itself,
// and registers the logger as a peer.
bool espnow_comm_init(const uint8_t *logger_mac);

// Sends the distance reading to the logger.
// Logger must have been registered as a peer via espnow_comm_init() first.
bool espnow_comm_send_data(uint16_t distance_mm);

// Opens a short RX window to listen for a drift-correction packet from
// the logger. Returns true if a correction was received (and writes the
// adjusted interval in seconds to *adjusted_interval_out); false if the
// window timed out with nothing received (not an error — just means no
// correction was needed/sent this cycle).
bool espnow_comm_listen_for_correction(uint32_t timeout_ms, uint32_t *adjusted_interval_out);

// Provisioning: for a sensor that doesn't yet have an interval assigned
// (see NVS-backed state in sensor_node.c). Repeatedly announces itself to
// the logger (once every announce_interval_sec) and listens for a reply,
// for up to timeout_sec total, radio active the whole time. Returns true
// and fills in *interval_sec_out / *start_delay_sec_out if the logger
// acknowledged; false if timeout_sec elapsed with no reply (caller should
// fall back to a conservative sleep and retry on the next wake, rather
// than looping forever and draining the battery).
bool espnow_comm_run_provisioning(uint32_t timeout_sec, uint32_t announce_interval_sec,
                                   uint32_t *interval_sec_out, uint32_t *start_delay_sec_out);

// Tears down ESP-NOW + WiFi. Call before deep sleep.
void espnow_comm_deinit(void);

#ifdef __cplusplus
}
#endif
