#include "espnow_comm.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stddef.h>
#include <string.h>
#include <sys/time.h>

static const char *TAG = "logger_espnow_comm";

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

// Real bounded queues, not a single-slot "latest value" mailbox -- with a
// mailbox, a second reading/event processed before pc_comm_task's next
// poll silently overwrites the first with no drop indication anywhere.
// Sized generously since each entry is tiny and pc_comm_task drains one
// per loop iteration (every ~50ms), well ahead of how often sensors report.
#define ESPNOW_COMM_READING_QUEUE_LEN 20
#define ESPNOW_COMM_PROVISIONING_QUEUE_LEN 10
static QueueHandle_t s_reading_queue = NULL;
static QueueHandle_t s_provisioning_queue = NULL;

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

int espnow_comm_get_rx_queue_depth(void) {
    return s_rx_queue ? (int)uxQueueMessagesWaiting(s_rx_queue) : 0;
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

// Sensors added at runtime via espnow_comm_add_sensor() (as opposed to
// the compiled-in list espnow_comm_init() is called with) are persisted
// here, so a logger reboot can re-register them without the PC having to
// send ADDSENSOR again for each one. Just the MAC list -- interval/anchor
// for a newly (re-)registered sensor always starts at the current default,
// same as any first-time ADDSENSOR.
#define ADDED_SENSORS_NVS_NAMESPACE "added_sensors"
#define ADDED_SENSORS_NVS_KEY "macs"

static void save_added_sensor_macs(const uint8_t macs[][6], int count) {
    nvs_handle_t handle;
    if (nvs_open(ADDED_SENSORS_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS to persist added sensor list");
        return;
    }
    nvs_set_blob(handle, ADDED_SENSORS_NVS_KEY, macs, (size_t)count * 6);
    nvs_commit(handle);
    nvs_close(handle);
}

// Registers `mac` as an ESP-NOW peer and gives it a slot in s_sensors,
// without touching NVS -- shared by espnow_comm_add_sensor() (which does
// persist) and load_added_sensors() at boot (which is replaying what's
// already in NVS, so re-saving it would be a no-op busywork write on
// every single startup).
static espnow_add_sensor_result_t register_sensor_slot(const uint8_t mac[6]) {
    if (find_sensor_slot(mac) >= 0) {
        return ESPNOW_ADD_SENSOR_ALREADY_REGISTERED;
    }
    if (s_sensor_count >= ESPNOW_COMM_MAX_SENSORS) {
        return ESPNOW_ADD_SENSOR_TABLE_FULL;
    }

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK) {
        return ESPNOW_ADD_SENSOR_PEER_FAILED;
    }

    sensor_state_t *sensor = &s_sensors[s_sensor_count++];
    memcpy(sensor->mac, mac, 6);
    sensor->interval_sec = s_default_interval_sec;
    sensor->anchor_epoch = s_default_anchor_epoch;
    sensor->last_seen = 0;
    return ESPNOW_ADD_SENSOR_OK;
}

// Called once during espnow_comm_init(), after the compiled-in sensor
// list has already claimed its slots -- re-registers whatever was
// previously added at runtime via espnow_comm_add_sensor(). A mac that's
// meanwhile been added to the compiled-in list too is simply skipped
// (register_sensor_slot's own dedup), so promoting a discovered sensor
// into the hardcoded list later is safe and doesn't need any cleanup.
static void load_added_sensors(void) {
    nvs_handle_t handle;
    if (nvs_open(ADDED_SENSORS_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return; // never persisted anything yet -- nothing to load
    }
    uint8_t macs[ESPNOW_COMM_MAX_SENSORS][6];
    size_t len = sizeof(macs);
    esp_err_t err = nvs_get_blob(handle, ADDED_SENSORS_NVS_KEY, macs, &len);
    nvs_close(handle);
    if (err != ESP_OK) {
        return;
    }
    int count = (int)(len / 6);
    for (int i = 0; i < count; i++) {
        espnow_add_sensor_result_t result = register_sensor_slot(macs[i]);
        if (result == ESPNOW_ADD_SENSOR_OK) {
            ESP_LOGI(TAG, "Restored previously-added sensor %02x:%02x:%02x:%02x:%02x:%02x from NVS",
                     macs[i][0], macs[i][1], macs[i][2], macs[i][3], macs[i][4], macs[i][5]);
        }
    }
}

espnow_add_sensor_result_t espnow_comm_add_sensor(const uint8_t mac[6]) {
    espnow_add_sensor_result_t result = register_sensor_slot(mac);
    if (result != ESPNOW_ADD_SENSOR_OK) {
        return result;
    }

    // Persist the updated "added at runtime" list: whatever NVS already
    // had, plus this new MAC. Re-reading NVS instead of keeping a second
    // parallel in-memory array avoids two sources of truth for the same list.
    uint8_t macs[ESPNOW_COMM_MAX_SENSORS][6];
    size_t len = 0;
    nvs_handle_t handle;
    if (nvs_open(ADDED_SENSORS_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        len = sizeof(macs);
        if (nvs_get_blob(handle, ADDED_SENSORS_NVS_KEY, macs, &len) != ESP_OK) {
            len = 0;
        }
        nvs_close(handle);
    }
    int count = (int)(len / 6);
    memcpy(macs[count++], mac, 6);
    save_added_sensor_macs(macs, count);

    return ESPNOW_ADD_SENSOR_OK;
}

espnow_remove_sensor_result_t espnow_comm_remove_sensor(const uint8_t mac[6]) {
    int slot = find_sensor_slot(mac);
    if (slot < 0) {
        return ESPNOW_REMOVE_SENSOR_NOT_FOUND;
    }

    esp_now_del_peer(mac);

    // Shift everything after slot down by one to keep s_sensors packed --
    // espnow_comm_get_sensor_info() and pc_comm_send_all_freq/sensor_list
    // iterate 0..count-1.
    for (int i = slot; i < s_sensor_count - 1; i++) {
        s_sensors[i] = s_sensors[i + 1];
    }
    s_sensor_count--;

    // If this mac was added at runtime (persisted via
    // espnow_comm_add_sensor()), drop it from NVS too so it doesn't come
    // back on the logger's next reboot. A mac from the compiled-in list
    // simply won't be found here -- that's expected, since this is a
    // runtime-only removal, not a permanent deregistration.
    uint8_t macs[ESPNOW_COMM_MAX_SENSORS][6];
    size_t len = 0;
    nvs_handle_t handle;
    if (nvs_open(ADDED_SENSORS_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        len = sizeof(macs);
        if (nvs_get_blob(handle, ADDED_SENSORS_NVS_KEY, macs, &len) != ESP_OK) {
            len = 0;
        }
        nvs_close(handle);
    }
    int count = (int)(len / 6);
    for (int i = 0; i < count; i++) {
        if (memcmp(macs[i], mac, 6) == 0) {
            for (int j = i; j < count - 1; j++) {
                memcpy(macs[j], macs[j + 1], 6);
            }
            save_added_sensor_macs(macs, count - 1);
            break;
        }
    }

    return ESPNOW_REMOVE_SENSOR_OK;
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

    // Logs every packet the radio hands us, before any filtering -- the
    // fastest way to tell "sensor never transmitted" apart from "it
    // transmitted but got dropped/overwritten downstream".
    ESP_EARLY_LOGI(TAG, "RX from %02x:%02x:%02x:%02x:%02x:%02x type=%d len=%d",
                   info->src_addr[0], info->src_addr[1], info->src_addr[2],
                   info->src_addr[3], info->src_addr[4], info->src_addr[5], type, len);

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

    if (xQueueSendFromISR(s_rx_queue, &item, NULL) != pdTRUE) { // safe to call from a driver callback context
        ESP_EARLY_LOGW(TAG, "rx_queue full, dropped packet from %02x:%02x:%02x:%02x:%02x:%02x type=%d",
                       info->src_addr[0], info->src_addr[1], info->src_addr[2],
                       info->src_addr[3], info->src_addr[4], info->src_addr[5], type);
    }
}

// This is where the actual work happens: drift calculation, sending the
// correction (or provisioning ack) back, and pushing onto the reading /
// provisioning-event queues that pc_comm_task drains.
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
                ESP_LOGI(TAG, "Processed reading from %02x:%02x:%02x:%02x:%02x:%02x: %d mm, next wake in %lu sec, rx_queue depth=%d",
                         item.src_mac[0], item.src_mac[1], item.src_mac[2], item.src_mac[3], item.src_mac[4], item.src_mac[5],
                         item.distance_mm, (unsigned long)adjusted_interval, (int)uxQueueMessagesWaiting(s_rx_queue));

                correction_packet_t corr = { .type = PKT_CORRECTION, .adjusted_interval_sec = adjusted_interval };
                esp_now_send(item.src_mac, (uint8_t *)&corr, sizeof(corr));

                latest_reading_t reading;
                memcpy(reading.mac, item.src_mac, 6);
                reading.distance_mm = item.distance_mm;
                if (xQueueSend(s_reading_queue, &reading, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "reading_queue full, dropped reading from %02x:%02x:%02x:%02x:%02x:%02x",
                             item.src_mac[0], item.src_mac[1], item.src_mac[2],
                             item.src_mac[3], item.src_mac[4], item.src_mac[5]);
                }

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

                if (xQueueSend(s_provisioning_queue, item.src_mac, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "provisioning_queue full, dropped event from %02x:%02x:%02x:%02x:%02x:%02x",
                             item.src_mac[0], item.src_mac[1], item.src_mac[2],
                             item.src_mac[3], item.src_mac[4], item.src_mac[5]);
                }
            }
        }
    }
}

bool espnow_comm_get_latest_reading(uint8_t *mac_out, uint16_t *distance_mm_out) {
    latest_reading_t reading;
    if (xQueueReceive(s_reading_queue, &reading, 0) != pdTRUE) {
        return false;
    }
    memcpy(mac_out, reading.mac, 6);
    *distance_mm_out = reading.distance_mm;
    return true;
}

bool espnow_comm_get_latest_provisioning_event(uint8_t *mac_out) {
    return xQueueReceive(s_provisioning_queue, mac_out, 0) == pdTRUE;
}

void espnow_comm_start_processing_task(void) {
    s_rx_queue = xQueueCreate(10, sizeof(rx_item_t)); // holds up to 10 pending packets
    s_reading_queue = xQueueCreate(ESPNOW_COMM_READING_QUEUE_LEN, sizeof(latest_reading_t));
    s_provisioning_queue = xQueueCreate(ESPNOW_COMM_PROVISIONING_QUEUE_LEN, 6); // 6-byte MAC per event
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
    esp_err_t tx_power_ret = esp_wifi_set_max_tx_power(40);
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

    // Re-register anything that was added at runtime (via ADDSENSOR) on a
    // previous boot, so it survives a logger reboot without needing the
    // PC to send ADDSENSOR again.
    load_added_sensors();

    return true;
}
