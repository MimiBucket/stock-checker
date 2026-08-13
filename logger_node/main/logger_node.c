#include "pc_comm.h"
#include "espnow_comm.h"
#include "esp_log.h"

static const char *TAG = "logger";

static const uint8_t SENSOR_MACS[][6] = {
    {0x78, 0xE3, 0x6D, 0xDE, 0x9C, 0xD8}, // bin sensor 1
};

#define SENSOR_COUNT (sizeof(SENSOR_MACS) / sizeof(SENSOR_MACS[0]))

void app_main(void) {
    pc_comm_init();
    pc_comm_wait_for_initial_sync();

    if (!espnow_comm_init(SENSOR_MACS, SENSOR_COUNT)) {
        ESP_LOGE(TAG, "ESP-NOW init failed");
    }

    espnow_comm_start_processing_task();
    pc_comm_start_task();
}