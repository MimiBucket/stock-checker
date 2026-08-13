#include "tof_sensor.h"
#include "vl53l.hpp"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include <stdint.h>
#include <system_error>

static const char *TAG = "tof_sensor";

#define I2C_SDA_PIN GPIO_NUM_13
#define I2C_SCL_PIN GPIO_NUM_14

static i2c_master_bus_handle_t s_bus_handle = nullptr;
static espp::Vl53l *s_tof = nullptr;

bool tof_sensor_init(void) {
    // Configure the ESP32 I2C bus for the VL53L0X.
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

    // Create the I2C bus.
    if (i2c_new_master_bus(&bus_config, &s_bus_handle) != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed");
        return false;
    }

    // Instantiate the VL53L sensor.
    s_tof = new espp::Vl53l(espp::Vl53l::Config{
        .device_address = espp::Vl53l::DEFAULT_ADDRESS,
        .log_level = espp::Logger::Verbosity::WARN,
    });

    if (!s_tof) {
        ESP_LOGE(TAG, "Sensor not initialized");
        return false;
    }

    return true;
}

bool tof_sensor_read(uint16_t *distance_mm_out) {
    std::error_code ec;
    uint16_t distance_mm = s_tof->get_distance_mm(ec);

    if (ec) {
        ESP_LOGW(TAG, "ToF read failed: %s", ec.message().c_str());
        return false;
    }

    *distance_mm_out = distance_mm;
    ESP_LOGI(TAG, "ToF read: %u mm", distance_mm);
    return true;
}

void tof_sensor_deinit(void) {
    if (s_tof) {
        delete s_tof;
        s_tof = nullptr;
    }
    if (s_bus_handle) {
        i2c_del_master_bus(s_bus_handle);
        s_bus_handle = nullptr;
    }
}