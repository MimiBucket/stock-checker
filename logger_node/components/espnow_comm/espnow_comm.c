#include "espnow_comm.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stddef.h>
#include <string.h>
#include <sys/time.h>

static const char *TAG = "espnow_comm";

// Every packet on the wire starts with one of these, so the receiver
// (logger or sensor) can tell what it's looking at instead of guessing
// from length alone. Mirrored exactly in the sensor's espnow_comm.cpp --
// if you change one side, change the other.
typedef enum {
    PKT_SENSOR_DATA = 1,       // sensor -> logger: a reading
    PKT_CORRECTION = 2,        // logger -> sensor: drift-corrected next interval (normal operation)
    PKT_PROVISION_REQUEST = 3, // sensor -> logger: "I don't have an interval yet, assign me one"
    PKT_PROVISION_ACK = 4,     // logger -> sensor: assigned interval (+ optional start delay)
} espnow_packet_type_t;

// packed so the wire format is byte-for-byte identical regardless of
// compiler padding choices -- both sides are ESP32/Xtensa with the same
// toolchain today, but this removes any doubt.
typedef struct __attribute__((packed)) {
    uint8_t type; // PKT_SENSOR_DATA
    uint16_t distance_mm;
} sensor_data_packet_t;

typedef struct __attribute__((packed)) {
    uint8_t type; // PKT_CORRECTION
    uint32_t adjusted_interval_sec;
} correction_packet_t;

typedef struct __attribute__((packed)) {
    uint8_t type; // PKT_PROVISION_REQUEST
} provision_request_packet_t;

typedef struct __attribute__((packed)) {
    uint8_t type; // PKT_PROVISION_ACK
    uint32_t interval_sec;
    uint32_t start_delay_sec;
} provision_ack_packet_t;

// What the receive callback pushes into the queue — which sensor, what
// kind of packet, and (for a PKT_SENSOR_DATA packet) the reading itself.
typedef struct {
    uint8_t src_mac[6];
    uint8_t type;
    uint16_t distance_mm; // only meaningful when type == PKT_SENSOR_DATA
} rx_item_t;

// What the processing task exposes as "latest reading" for pc_comm_task to pick up.
typedef struct {
    uint8_t mac[6];
    uint16_t distance_mm;
} latest_reading_t;

// Per-sensor state, one entry per MAC passed to espnow_comm_init(). A
// small fixed-size table (not one shared global) is what makes the
// schedule math work correctly with more than one sensor -- each one has
// its own interval/anchor.
#define ESPNOW_COMM_MAX_SENSORS 20

typedef struct {
    uint8_t mac[6];
    uint32_t interval_sec;
    // Wall-clock reference point that every wake is aligned to: valid
    // wake times are anchor_epoch + k*interval_sec for integer k. Using
    // an absolute anchor (rather than "next = last + interval") means
    // the target time for the sensor's next wake can always be recomputed
    // fresh from (anchor, interval, now) -- no per-cycle state to carry
    // forward, and no way for correction error to accumulate over many
    // cycles the way it could with an incrementally-updated expectation.
    // Defaults to 0 (the Unix epoch), which isn't arbitrary: since it's
    // exactly on an hour/minute/day boundary, it makes every sensor
    // naturally wall-clock-aligned (e.g. every hour on the hour) even
    // before anyone sets an explicit anchor via SETFREQ.
    time_t anchor_epoch;
    time_t last_seen; // 0 if never seen
} sensor_state_t;

static sensor_state_t s_sensors[ESPNOW_COMM_MAX_SENSORS];
static int s_sensor_count = 0;

// Interval/anchor used for any sensor that hasn't been individually
// configured via a targeted SETFREQ -- also what a never-before-seen (to
// us) sensor gets handed the first time it asks to be provisioned.
static uint32_t s_default_interval_sec = 30;
static time_t s_default_anchor_epoch = 0;

static QueueHandle_t s_rx_queue = NULL;
static volatile bool s_new_reading_available = false;
static latest_reading_t s_latest_reading;

static volatile bool s_provisioning_event_available = false;
static uint8_t s_provisioning_event_mac[6];

static int find_sensor_slot(const uint8_t mac[6]) {
    for (int i = 0; i < s_sensor_count; i++) {
        if (memcmp(s_sensors[i].mac, mac, 6) == 0) {
            return i;
        }
    }
    return -1;
}

// Seconds until the next scheduled wake strictly after `now`, for a
// sensor whose valid wake times are anchor_epoch + k*interval_sec. Used
// for both the ongoing PKT_CORRECTION reply and the initial
// PKT_PROVISION_ACK's start_delay_sec, so a sensor always ends up on the
// same aligned grid regardless of what moment it happened to check in.
static uint32_t seconds_until_next_slot(uint32_t interval_sec, time_t anchor_epoch, time_t now) {
    if (interval_sec == 0) {
        return 1; // guard against div-by-zero; shouldn't happen (SETFREQ/provisioning reject 0)
    }
    int64_t elapsed = (int64_t)now - (int64_t)anchor_epoch;
    // Floor division so this works correctly even when `now` is before
    // the anchor (elapsed negative) -- plain C integer division
    // truncates toward zero, which would pick the wrong slot there.
    int64_t last_slot_index = elapsed / interval_sec;
    if (elapsed % interval_sec != 0 && elapsed < 0) {
        last_slot_index--;
    }
    time_t next_wake = (time_t)(anchor_epoch + (last_slot_index + 1) * (int64_t)interval_sec);
    int64_t sleep_sec = (int64_t)next_wake - (int64_t)now;
    return (sleep_sec > 0) ? (uint32_t)sleep_sec : 1;
}

void espnow_comm_set_schedule(const uint8_t mac[6], uint32_t interval_sec, time_t anchor_epoch) {
    if (mac == NULL) {
        s_default_interval_sec = interval_sec;
        s_default_anchor_epoch = anchor_epoch;
        for (int i = 0; i < s_sensor_count; i++) {
            s_sensors[i].interval_sec = interval_sec;
            s_sensors[i].anchor_epoch = anchor_epoch;
        }
        return;
    }
    int slot = find_sensor_slot(mac);
    if (slot >= 0) {
        s_sensors[slot].interval_sec = interval_sec;
        s_sensors[slot].anchor_epoch = anchor_epoch;
    } else {
        ESP_LOGW(TAG, "SETFREQ target not a registered sensor, ignoring");
    }
}

void espnow_comm_get_schedule(const uint8_t mac[6], uint32_t *interval_sec_out, time_t *anchor_epoch_out) {
    int slot = (mac != NULL) ? find_sensor_slot(mac) : -1;
    if (slot >= 0) {
        *interval_sec_out = s_sensors[slot].interval_sec;
        *anchor_epoch_out = s_sensors[slot].anchor_epoch;
    } else {
        *interval_sec_out = s_default_interval_sec;
        *anchor_epoch_out = s_default_anchor_epoch;
    }
}

int espnow_comm_get_sensor_count(void) {
    return s_sensor_count;
}

bool espnow_comm_get_sensor_info(int index, uint8_t mac_out[6], uint32_t *interval_sec_out, time_t *anchor_epoch_out) {
    if (index < 0 || index >= s_sensor_count) {
        return false;
    }
    memcpy(mac_out, s_sensors[index].mac, 6);
    *interval_sec_out = s_sensors[index].interval_sec;
    *anchor_epoch_out = s_sensors[index].anchor_epoch;
    return true;
}

// Kept deliberately trivial — just copies the packet off the driver's
// buffer and pushes it into the queue. No math, no esp_now_send() here,
// so this callback returns fast regardless of how busy the processing
// task currently is.
static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len < 1 || !s_rx_queue) {
        return;
    }
    uint8_t type = data[0];

    rx_item_t item = {0};
    memcpy(item.src_mac, info->src_addr, 6);
    item.type = type;

    if (type == PKT_SENSOR_DATA && len == sizeof(sensor_data_packet_t)) {
        sensor_data_packet_t pkt;
        memcpy(&pkt, data, sizeof(pkt));
        item.distance_mm = pkt.distance_mm;
    } else if (type != PKT_PROVISION_REQUEST || len != sizeof(provision_request_packet_t)) {
        return; // unrecognized or malformed packet, drop it
    }

    xQueueSendFromISR(s_rx_queue, &item, NULL); // safe to call from a driver callback context
}

// This is where the actual work happens: drift calculation, sending the
// correction (or provisioning ack) back, and updating the "latest
// reading" / "latest provisioning event" mailboxes for pc_comm_task.
void espnow_process_task(void *arg) {
    rx_item_t item;
    while (1) {
        if (xQueueReceive(s_rx_queue, &item, portMAX_DELAY) == pdTRUE) {
            int slot = find_sensor_slot(item.src_mac);
            if (slot < 0) {
                ESP_LOGW(TAG, "Ignoring packet from unregistered sensor %02x:%02x:%02x:%02x:%02x:%02x",
                         item.src_mac[0], item.src_mac[1], item.src_mac[2],
                         item.src_mac[3], item.src_mac[4], item.src_mac[5]);
                continue;
            }
            sensor_state_t *sensor = &s_sensors[slot];

            struct timeval now;
            gettimeofday(&now, NULL);
            time_t actual_arrival = now.tv_sec;
            sensor->last_seen = actual_arrival;

            if (item.type == PKT_SENSOR_DATA) {
                uint32_t adjusted_interval = seconds_until_next_slot(sensor->interval_sec, sensor->anchor_epoch, actual_arrival);
                ESP_LOGI(TAG, "Processed reading from %02x:%02x:...: %d mm, next wake in %lu sec",
                         item.src_mac[0], item.src_mac[1], item.distance_mm, (unsigned long)adjusted_interval);

                correction_packet_t corr = { .type = PKT_CORRECTION, .adjusted_interval_sec = adjusted_interval };
                esp_now_send(item.src_mac, (uint8_t *)&corr, sizeof(corr));

                memcpy(s_latest_reading.mac, item.src_mac, 6);
                s_latest_reading.distance_mm = item.distance_mm;
                s_new_reading_available = true;

            } else if (item.type == PKT_PROVISION_REQUEST) {
                // Align a freshly-provisioned sensor to the schedule grid
                // immediately, same as any other check-in -- rather than
                // always "start now", it waits just long enough to land
                // on the next anchor_epoch + k*interval_sec slot.
                uint32_t start_delay = seconds_until_next_slot(sensor->interval_sec, sensor->anchor_epoch, actual_arrival);
                provision_ack_packet_t ack = {
                    .type = PKT_PROVISION_ACK,
                    .interval_sec = sensor->interval_sec,
                    .start_delay_sec = start_delay,
                };
                esp_now_send(item.src_mac, (uint8_t *)&ack, sizeof(ack));

                ESP_LOGI(TAG, "Provisioned sensor %02x:%02x:...: interval %lu sec, first wake in %lu sec",
                         item.src_mac[0], item.src_mac[1], (unsigned long)sensor->interval_sec, (unsigned long)start_delay);

                memcpy(s_provisioning_event_mac, item.src_mac, 6);
                s_provisioning_event_available = true;
            }
        }
    }
}

bool espnow_comm_get_latest_reading(uint8_t *mac_out, uint16_t *distance_mm_out) {
    if (!s_new_reading_available) {
        return false;
    }
    memcpy(mac_out, s_latest_reading.mac, 6);
    *distance_mm_out = s_latest_reading.distance_mm;
    s_new_reading_available = false; // consumed
    return true;
}

bool espnow_comm_get_latest_provisioning_event(uint8_t *mac_out) {
    if (!s_provisioning_event_available) {
        return false;
    }
    memcpy(mac_out, s_provisioning_event_mac, 6);
    s_provisioning_event_available = false; // consumed
    return true;
}

void espnow_comm_start_processing_task(void) {
    s_rx_queue = xQueueCreate(10, sizeof(rx_item_t)); // holds up to 10 pending packets
    xTaskCreate(espnow_process_task, "espnow_process_task", 4096, NULL, 5, NULL);
}

bool espnow_comm_init(const uint8_t sensor_macs[][6], int count) {
    if (count > ESPNOW_COMM_MAX_SENSORS) {
        ESP_LOGE(TAG, "count (%d) exceeds ESPNOW_COMM_MAX_SENSORS (%d), clamping",
                 count, ESPNOW_COMM_MAX_SENSORS);
        count = ESPNOW_COMM_MAX_SENSORS;
    }

    s_sensor_count = count;
    for (int i = 0; i < count; i++) {
        memcpy(s_sensors[i].mac, sensor_macs[i], 6);
        s_sensors[i].interval_sec = s_default_interval_sec;
        s_sensors[i].anchor_epoch = s_default_anchor_epoch;
        s_sensors[i].last_seen = 0;
    }

    // WiFi (and thus ESP-NOW, which rides on the WiFi driver) stores
    // calibration/config data in NVS, so it must be initialized first.
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    esp_netif_init();
    esp_event_loop_create_default();
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&wifi_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed");
        return false;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    // TEMP: cap TX power to shrink the current spike when the radio powers
    // on -- on marginal USB supplies that spike browns out the board.
    // Trades ESP-NOW range for stability; remove once the power supply
    // (cable/capacitor) is fixed. 44 = 11 dBm (default max is ~20 dBm).
    esp_err_t tx_power_ret = esp_wifi_set_max_tx_power(44);
    if (tx_power_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to reduce TX power: %d", tx_power_ret);
    }

    if (esp_now_init() != ESP_OK) {
        ESP_LOGE(TAG, "ESP-NOW init failed");
        return false;
    }

    esp_now_register_recv_cb(on_data_recv);

    // Register every sensor in the caller-provided list as a peer.
    for (int i = 0; i < count; i++) {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, sensor_macs[i], 6);
        peer.channel = 0;
        peer.encrypt = false;
        if (esp_now_add_peer(&peer) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add peer %d", i);
            return false;
        }
    }

    return true;
}
