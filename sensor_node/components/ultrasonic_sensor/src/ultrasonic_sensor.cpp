#include "ultrasonic_sensor.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "ultrasonic_sensor";

// Placeholder wiring for an HC-SR04-style trig/echo sensor. Chosen to
// avoid the ToF sensor's I2C pins (GPIO13/14, see tof_sensor.cpp) and the
// status LED (GPIO27), so both sensors can stay wired up at once for
// comparison. Update to match your actual wiring.
#define TRIG_PIN GPIO_NUM_25
#define ECHO_PIN GPIO_NUM_26

#define TRIG_PULSE_US 10

// Longest ping-to-echo window we'll wait, for both "no echo started" and
// "echo never ended" (out of range / sensor not connected). ~5 m round
// trip at 343 m/s is far more range than a stock bin needs.
#define ECHO_TIMEOUT_US 30000

#define SPEED_OF_SOUND_MM_PER_US 0.343f

bool ultrasonic_sensor_init(void)
{
    gpio_config_t trig_conf = {
        .pin_bit_mask = (1ULL << TRIG_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&trig_conf) != ESP_OK) {
        ESP_LOGE(TAG, "TRIG GPIO config failed");
        return false;
    }
    gpio_set_level(TRIG_PIN, 0);

    gpio_config_t echo_conf = {
        .pin_bit_mask = (1ULL << ECHO_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&echo_conf) != ESP_OK) {
        ESP_LOGE(TAG, "ECHO GPIO config failed");
        return false;
    }

    ESP_LOGI(TAG, "Ultrasonic sensor ready (TRIG=%d, ECHO=%d)", TRIG_PIN, ECHO_PIN);
    return true;
}

bool ultrasonic_sensor_read(uint16_t *distance_mm_out)
{
    gpio_set_level(TRIG_PIN, 1);
    esp_rom_delay_us(TRIG_PULSE_US);
    gpio_set_level(TRIG_PIN, 0);

    int64_t wait_start = esp_timer_get_time();
    while (gpio_get_level(ECHO_PIN) == 0) {
        if (esp_timer_get_time() - wait_start > ECHO_TIMEOUT_US) {
            ESP_LOGW(TAG, "Timed out waiting for echo to start");
            return false;
        }
    }

    int64_t echo_start = esp_timer_get_time();
    while (gpio_get_level(ECHO_PIN) == 1) {
        if (esp_timer_get_time() - echo_start > ECHO_TIMEOUT_US) {
            ESP_LOGW(TAG, "Echo pulse exceeded timeout (out of range?)");
            return false;
        }
    }
    int64_t duration_us = esp_timer_get_time() - echo_start;

    *distance_mm_out = (uint16_t)((duration_us * SPEED_OF_SOUND_MM_PER_US) / 2.0f);

    ESP_LOGI(TAG, "Ultrasonic distance: %u mm (echo %lld us)",
             *distance_mm_out, (long long)duration_us);
    return true;
}

void ultrasonic_sensor_deinit(void)
{
    gpio_set_level(TRIG_PIN, 0);
    gpio_reset_pin(TRIG_PIN);
    gpio_reset_pin(ECHO_PIN);
}
