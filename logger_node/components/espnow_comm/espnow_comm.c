#include "espnow_comm.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <sys/time.h>

static const char *TAG = "espnow_comm";

static uint32_t s_expected_interval_sec = 3600;

typedef struct {
    uint16_t distance_mm;
} sensor_data_packet_t;

typedef struct {
    uint32_t adjusted_interval_sec;
} correction_packet_t;

// What the receive callback pushes into the queue — raw packet + who sent it.
typedef struct {
    uint8_t src_mac[6];
    sensor_data_packet_t data;
} rx_item_t;

// What the processing task exposes as "latest reading" for pc_comm_task to pick up.
typedef struct {
    uint8_t mac[6];
    uint16_t distance_mm;
} latest_reading_t;

static QueueHandle_t s_rx_queue = nullptr;
static volatile bool s_new_reading_available = false;
static latest_reading_t s_latest_reading;

void espnow_comm_set_expected_interval(uint32_t interval_sec) {
    s_expected_interval_sec = interval_sec;
}

// Kept deliberately trivial — just copies the packet off the driver's
// buffer and pushes it into the queue. No math, no esp_now_send() here,
// so this callback returns fast regardless of how busy the processing
// task currently is.
static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len != sizeof(sensor_data_packet_t) || !s_rx_queue) {
        return;
    }
    rx_item_t item;
    memcpy(item.src_mac, info->src_addr, 6);
    memcpy(&item.data, data, sizeof(item.data));
    xQueueSendFromISR(s_rx_queue, &item, NULL); // safe to call from a driver callback context
}

// This is where the actual work happens: drift calculation, sending the
// correction back, and updating the "latest reading" for pc_comm_task.
static time_t s_expected_arrival = 0; // set once time sync completes, advances by interval each cycle

static void espnow_process_task(void *arg) {
    rx_item_t item;
    while (1) {
        if (xQueueReceive(s_rx_queue, &item, portMAX_DELAY) == pdTRUE) {
            struct timeval now;
            gettimeofday(&now, NULL);
            time_t actual_arrival = now.tv_sec;

            int32_t drift_sec = (int32_t)(actual_arrival - s_expected_arrival);
            uint32_t adjusted_interval = s_expected_interval_sec - drift_sec;

            correction_packet_t corr = { .adjusted_interval_sec = adjusted_interval };
            esp_now_send(item.src_mac, (uint8_t *)&corr, sizeof(corr));

            ESP_LOGI(TAG, "Processed reading from %02x:%02x:...: %d mm, drift %ld sec",
                     item.src_mac[0], item.src_mac[1], item.data.distance_mm, (long)drift_sec);

            memcpy(s_latest_reading.mac, item.src_mac, 6);
            s_latest_reading.distance_mm = item.data.distance_mm;
            s_new_reading_available = true;

            // Advance the shared expected time for the next cycle.
            s_expected_arrival += s_expected_interval_sec;
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

void espnow_comm_start_processing_task(void) {
    s_rx_queue = xQueueCreate(10, sizeof(rx_item_t)); // holds up to 10 pending packets
    xTaskCreate(espnow_process_task, "espnow_process_task", 4096, NULL, 5, NULL);
}

bool espnow_comm_init(const uint8_t sensor_macs[][6], int count) {
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