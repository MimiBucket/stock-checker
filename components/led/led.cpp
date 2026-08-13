#include "led.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <system_error>

#define LED_GPIO_PIN GPIO_NUM_27

static const char *TAG = "led";

// Placeholder (Distance increases as the bin empties (less material between sensor and target))
#define LOW_STOCK_THRESHOLD_MM 150

bool led_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO_PIN), // bitmask selecting which pin(s) to configure
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,      
    };

    if (gpio_config(&io_conf) != ESP_OK) {
        ESP_LOGE(TAG, "LED GPIO config failed");
        return false;
    }
    return true;
}

void led_update(uint16_t distance_mm) {
    bool low_or_empty = (distance_mm >= LOW_STOCK_THRESHOLD_MM);
    gpio_set_level(LED_GPIO_PIN, low_or_empty ? 1 : 0);
}

void led_hold_for_sleep(void) {

}
