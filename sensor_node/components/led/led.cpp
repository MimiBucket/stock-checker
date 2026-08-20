#include "led.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define LED_GPIO_PIN GPIO_NUM_33

static const char *TAG = "led";

bool led_init(void) {
    // A hold from the previous led_hold_for_sleep() survives waking, and
    // gpio_config()/gpio_set_level() have no effect on a held pin -- release
    // it first; led_hold_for_sleep() re-enables it before the next sleep.
    gpio_hold_dis(LED_GPIO_PIN);

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

void led_update(bool low_or_empty) {
    gpio_set_level(LED_GPIO_PIN, low_or_empty ? 1 : 0);
}

void led_hold_for_sleep(void) {
    // Without this the pin floats during deep sleep and the LED drops out
    // every cycle. gpio_hold_en() latches its level through sleep;
    // gpio_deep_sleep_hold_en() is the chip-wide switch that respects it.
    gpio_hold_en(LED_GPIO_PIN);
    gpio_deep_sleep_hold_en();
}
