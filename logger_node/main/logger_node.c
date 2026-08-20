#include "pc_comm.h"
#include "espnow_comm.h"
#include "esp_log.h"

static const char *TAG = "logger";

// No compiled-in sensors for now -- testing whether the logger still
// browns out with zero ESP-NOW peers registered (isolates the WiFi/
// ESP-NOW radio power-on spike itself from any per-peer traffic).
#define SENSOR_COUNT 0

void app_main(void) {
    if (!espnow_comm_init(NULL, SENSOR_COUNT)) {
        ESP_LOGE(TAG, "ESP-NOW init failed");
    }

    pc_comm_init();

    espnow_comm_start_processing_task();

    pc_comm_wait_for_initial_sync();
    pc_comm_send_sensor_list();
    pc_comm_send_all_freq(); // one FREQ line per sensor, so the PC starts with an accurate picture
    pc_comm_send_threshold(); // so a reconnecting PC learns what's already set, not just its own UI default

    pc_comm_start_task();
}