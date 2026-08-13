#include <stdio.h>
#include "tof_sensor.h"
#include "led.h"
#include "espnow_comm.h"
#include "esp_sleep.h"
#include "esp_log.h"

static const char *TAG = "sensor_node";

void app_main(void) {
    led_init();
    espnow_comm_init((const uint8_t *)"\x10\x52\x1C\x60\x56\xC4"); // logger MAC address

    uint16_t distance_mm = 0;
    if (tof_sensor_init() && tof_sensor_read(&distance_mm)) {
        ESP_LOGI(TAG, "Distance: %d mm", distance_mm);
        led_update(distance_mm);
        espnow_comm_send_data(distance_mm);
    } 
    
    uint32_t adjusted_interval = 0;
    uint32_t sleep_duration_sec = 10; //60 * 60; // default to 3600 seconds (1 hour)
    if (espnow_comm_listen_for_correction(100, &adjusted_interval)) {
        ESP_LOGI(TAG, "Got correction: %lu sec", adjusted_interval);
        sleep_duration_sec = adjusted_interval;
    } else {
        ESP_LOGI(TAG, "No correction received this cycle");
    }

    tof_sensor_deinit();

    led_hold_for_sleep(); 
    ESP_LOGI(TAG, "Entering deep sleep");   
    esp_sleep_enable_timer_wakeup((uint64_t)sleep_duration_sec * 1000000ULL);
    esp_deep_sleep_start();
}