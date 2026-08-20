#include "pc_comm.h"
#include "espnow_comm.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "logger";

// TEMP diagnostic: BROWNOUT confirms the disconnects seen over the PC
// serial link are a power-rail sag, not a USB/host-side issue.
// Remove once root-caused. (mirrors sensor_node.c's log_reset_reason)
static void log_reset_reason(void) {
    esp_reset_reason_t reason = esp_reset_reason();
    const char *label = "?";
    switch (reason) {
        case ESP_RST_POWERON:   label = "POWERON (fresh power or EN reset)"; break;
        case ESP_RST_BROWNOUT:  label = "BROWNOUT"; break;
        case ESP_RST_SW:        label = "SW (esp_restart)"; break;
        case ESP_RST_PANIC:     label = "PANIC"; break;
        default: break;
    }
    ESP_LOGI(TAG, "Reset reason: %d (%s)", reason, label);
}

// No compiled-in sensors for now -- testing whether the logger still
// browns out with zero ESP-NOW peers registered (isolates the WiFi/
// ESP-NOW radio power-on spike itself from any per-peer traffic).
#define SENSOR_COUNT 0

void app_main(void) {
    log_reset_reason();

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