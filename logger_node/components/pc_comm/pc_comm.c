#include "pc_comm.h"
#include "espnow_comm.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/time.h>

static const char *TAG = "pc_comm";

#define UART_PORT UART_NUM_0
#define UART_BUF_SIZE 128

void pc_comm_init(void) {
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(UART_PORT, &uart_config);
    uart_driver_install(UART_PORT, UART_BUF_SIZE * 2, 0, 0, NULL, 0);
}

static bool check_and_apply_time(void) {
    uint8_t data[UART_BUF_SIZE];
    int len = uart_read_bytes(UART_PORT, data, UART_BUF_SIZE - 1, 0);
    if (len <= 0) return false;
    data[len] = '\0';

    if (strncmp((char *)data, "SETTIME ", 8) == 0) {
        long epoch_sec = atol((char *)data + 8);
        if (epoch_sec > 0) {
            struct timeval tv = { .tv_sec = epoch_sec, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "Time set from PC: %ld", epoch_sec);
            return true;
        }
    }
    return false;
}

void pc_comm_wait_for_initial_sync(void) {
    ESP_LOGI(TAG, "Waiting for time sync from PC...");
    while (!check_and_apply_time()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "Time synced.");
}

static void pc_comm_task(void *arg) {
    uint8_t mac[6];
    uint16_t distance_mm;

    while (1) {
        check_and_apply_time(); // periodic resync / future SETFREQ

        if (espnow_comm_get_latest_reading(mac, &distance_mm)) {
            char line[64];
            snprintf(line, sizeof(line), "DATA %02x:%02x:%02x:%02x:%02x:%02x %d\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], distance_mm);
            printf("%s", line);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void pc_comm_start_task(void) {
    xTaskCreate(pc_comm_task, "pc_comm_task", 4096, NULL, 5, NULL);
}