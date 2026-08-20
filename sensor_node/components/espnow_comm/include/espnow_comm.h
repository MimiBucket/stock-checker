#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initializes WiFi (station mode) + ESP-NOW, and adds the logger as a peer.
bool espnow_comm_init(const uint8_t *logger_mac);

// Sends the distance reading to the logger.
bool espnow_comm_send_data(uint16_t distance_mm);

// low_or_empty_out: the stock status the logger computed for the reading
// this reply answers (from the PC-configured thresholds), false == OK.
// The sensor doesn't know the threshold values themselves -- it just
// displays whatever the logger says.
//
// Opens a short RX window for a reply packet (drift-corrected interval +
// stock status) from the logger. True + *adjusted_interval_out/
// *low_or_empty_out if one arrived; false on timeout (not an error --
// just means none was sent this cycle).
bool espnow_comm_listen_for_reply(uint32_t timeout_ms, uint32_t *adjusted_interval_out, bool *low_or_empty_out);

// For a sensor with no interval assigned yet. Re-announces itself every
// announce_interval_sec until the logger replies or timeout_sec elapses.
// True + *interval_sec_out/*start_delay_sec_out on ack; false on timeout
// (caller should fall back to a conservative sleep and retry next wake).
bool espnow_comm_run_provisioning(uint32_t timeout_sec, uint32_t announce_interval_sec,
                                   uint32_t *interval_sec_out, uint32_t *start_delay_sec_out);

// Tears down ESP-NOW + WiFi. Call before deep sleep.
void espnow_comm_deinit(void);

#ifdef __cplusplus
}
#endif
