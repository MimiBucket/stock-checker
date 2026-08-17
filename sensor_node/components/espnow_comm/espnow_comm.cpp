#include "espnow_comm.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "esp_now.h"
#include "nvs_flash.h"

static const char *TAG = "sensor_espnow_comm";

// Every packet on the wire starts with one of these, so the receiver
// (logger or sensor) can tell what it's looking at instead of guessing
// from length alone. Mirrored exactly in the logger's espnow_comm.c --
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

static uint8_t s_logger_mac[6];

static volatile bool s_correction_received = false;
static uint32_t s_received_interval = 0;

static volatile bool s_provision_ack_received = false;
static uint32_t s_provision_interval = 0;
static uint32_t s_provision_start_delay = 0;

static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len < 1) {
        return;
    }
    uint8_t type = data[0];

    if (type == PKT_CORRECTION && len == sizeof(correction_packet_t)) {
        correction_packet_t pkt;
        memcpy(&pkt, data, sizeof(pkt));
        s_received_interval = pkt.adjusted_interval_sec;
        s_correction_received = true; // simple flag, checked by the polling loop below
    } else if (type == PKT_PROVISION_ACK && len == sizeof(provision_ack_packet_t)) {
        provision_ack_packet_t pkt;
        memcpy(&pkt, data, sizeof(pkt));
        s_provision_interval = pkt.interval_sec;
        s_provision_start_delay = pkt.start_delay_sec;
        s_provision_ack_received = true;
    }
}

bool espnow_comm_listen_for_correction(uint32_t timeout_ms, uint32_t *adjusted_interval_out) {
    s_correction_received = false;

    // Just poll the flag every 10ms until either it's set, or we time out.
    uint32_t waited_ms = 0;
    while (waited_ms < timeout_ms) {
        if (s_correction_received) {
            *adjusted_interval_out = s_received_interval;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        waited_ms += 10;
    }
    return false; // timed out, nothing received
}

static void send_provision_request(void) {
    provision_request_packet_t pkt = { .type = PKT_PROVISION_REQUEST };
    esp_now_send(s_logger_mac, (uint8_t *)&pkt, sizeof(pkt));
}

static bool poll_for_provision_ack(uint32_t timeout_ms, uint32_t *interval_sec_out, uint32_t *start_delay_sec_out) {
    s_provision_ack_received = false;
    uint32_t waited_ms = 0;
    while (waited_ms < timeout_ms) {
        if (s_provision_ack_received) {
            *interval_sec_out = s_provision_interval;
            *start_delay_sec_out = s_provision_start_delay;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        waited_ms += 10;
    }
    return false;
}

bool espnow_comm_run_provisioning(uint32_t timeout_sec, uint32_t announce_interval_sec,
                                   uint32_t *interval_sec_out, uint32_t *start_delay_sec_out) {
    uint32_t elapsed_sec = 0;
    while (elapsed_sec < timeout_sec) {
        ESP_LOGI(TAG, "Waiting for interval setting...");
        send_provision_request();
        if (poll_for_provision_ack(announce_interval_sec * 1000, interval_sec_out, start_delay_sec_out)) {
            ESP_LOGI(TAG, "Provisioning successful: interval=%u sec, start_delay=%u sec", *interval_sec_out, *start_delay_sec_out);
            return true;
        }
        elapsed_sec += announce_interval_sec;
    }
    return false; // timed out -- caller falls back to a conservative sleep and retries next wake
}

bool espnow_comm_init(const uint8_t *logger_mac) {
    memcpy(s_logger_mac, logger_mac, 6);

    // Initialize NVS flash partition (required before WiFi init)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGI(TAG, "NVS partition corrupted, erasing...");
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS flash init failed: %s", esp_err_to_name(ret));
        return false;
    }

    esp_netif_init();
    esp_event_loop_create_default();
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&wifi_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed");
        return false;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    if (esp_now_init() != ESP_OK) {
        ESP_LOGE(TAG, "ESP-NOW init failed");
        return false;
    }

    esp_now_register_recv_cb(on_data_recv);

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, s_logger_mac, 6);
    peer.channel = 0;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add logger as peer");
        return false;
    }

    return true;
}

bool espnow_comm_send_data(uint16_t distance_mm) {
    sensor_data_packet_t pkt = { .type = PKT_SENSOR_DATA, .distance_mm = distance_mm };
    esp_err_t err = esp_now_send(s_logger_mac, (uint8_t *)&pkt, sizeof(pkt));
    ESP_LOGI(TAG, "Sent distance: %u mm", distance_mm);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Send failed");
        return false;
    }
    return true;
}

void espnow_comm_deinit(void) {
    esp_now_deinit();
    esp_wifi_stop();
    esp_wifi_deinit();
}
