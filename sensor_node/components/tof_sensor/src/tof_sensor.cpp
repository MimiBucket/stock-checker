#include "tof_sensor.h"
extern "C" {
#include "VL53L1X_api.h"
#include "vl53l1_platform.h"
}
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include "esp_log.h"

static const char *TAG = "tof_sensor";

#define I2C_SDA_PIN GPIO_NUM_13
#define I2C_SCL_PIN GPIO_NUM_14

// ESP-IDF uses the 7-bit I2C address
#define VL53L1X_ADDRESS 0x29

// ST API uses the 8-bit device address
#define VL53L1X_DEV 0x52

static i2c_master_bus_handle_t s_bus_handle = NULL;
static i2c_master_dev_handle_t s_tof_i2c = NULL;


bool tof_sensor_init(void)
{
    // Configure I2C bus
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = true,
            .allow_pd = false,
        },
    };

    // Create I2C bus
    if (i2c_new_master_bus(&bus_config, &s_bus_handle) != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed");
        return false;
    }

    // Add VL53L1X to I2C bus
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = VL53L1X_ADDRESS,
        .scl_speed_hz = 400000,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = false,
        },
    };

    if (i2c_master_bus_add_device(
            s_bus_handle,
            &dev_config,
            &s_tof_i2c) != ESP_OK) {

        ESP_LOGE(TAG, "Failed to add VL53L1X to I2C bus");
        return false;
    }

    // Give the ST platform layer access to the ESP-IDF I2C device
    vl53l1_platform_set_i2c_device(s_tof_i2c);

    ESP_LOGI(TAG, "VL53L1X I2C device added");

    // Check sensor boot state
    uint8_t boot_state = 0;

    if (VL53L1X_BootState(VL53L1X_DEV, &boot_state)
            != VL53L1X_ERROR_NONE) {

        ESP_LOGE(TAG, "Failed to read VL53L1X boot state");
        return false;
    }

    ESP_LOGI(TAG, "VL53L1X boot state: %u", boot_state);

    // Initialize sensor using ST API
    if (VL53L1X_SensorInit(VL53L1X_DEV)
            != VL53L1X_ERROR_NONE) {

        ESP_LOGE(TAG, "VL53L1X sensor initialization failed");
        return false;
    }

    ESP_LOGI(TAG, "VL53L1X initialized successfully");

    // Set timing budget
    if (VL53L1X_SetTimingBudgetInMs(VL53L1X_DEV, 100)
            != VL53L1X_ERROR_NONE) {

        ESP_LOGE(TAG, "Failed to set timing budget");
        return false;
    }

    // Start continuous ranging
    if (VL53L1X_StartRanging(VL53L1X_DEV)
            != VL53L1X_ERROR_NONE) {

        ESP_LOGE(TAG, "Failed to start ranging");
        return false;
    }

    ESP_LOGI(TAG, "VL53L1X ranging started");

    return true;
}


bool tof_sensor_read(uint16_t *distance_mm_out)
{
    uint8_t ready = 0;

    // Wait for measurement to become ready
    for (int i = 0; i < 25; i++) {

        if (VL53L1X_CheckForDataReady(VL53L1X_DEV, &ready)
                != VL53L1X_ERROR_NONE) {

            ESP_LOGE(TAG, "Failed to check data ready");
            return false;
        }

        if (ready) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (!ready) {
        ESP_LOGW(TAG, "VL53L1X measurement timeout");
        return false;
    }

    // Read distance
    if (VL53L1X_GetDistance(
            VL53L1X_DEV,
            distance_mm_out)
            != VL53L1X_ERROR_NONE) {

        ESP_LOGE(TAG, "Failed to read distance");
        return false;
    }

    ESP_LOGI(TAG, "ToF distance: %u mm", *distance_mm_out);

    // Clear interrupt for next measurement
    if (VL53L1X_ClearInterrupt(VL53L1X_DEV)
            != VL53L1X_ERROR_NONE) {

        ESP_LOGE(TAG, "Failed to clear interrupt");
        return false;
    }

    return true;
}


void tof_sensor_deinit(void)
{
    VL53L1X_StopRanging(VL53L1X_DEV);

    if (s_tof_i2c) {
        i2c_master_bus_rm_device(s_tof_i2c);
        s_tof_i2c = NULL;
    }

    if (s_bus_handle) {
        i2c_del_master_bus(s_bus_handle);
        s_bus_handle = NULL;
    }
}