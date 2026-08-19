#include "led.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define LED_GPIO_PIN GPIO_NUM_33

static const char *TAG = "led";

// Placeholder (Distance increases as the bin empties (less material between sensor and target))
#define LOW_STOCK_THRESHOLD_MM 30

bool led_init(void) {
    // If the previous cycle called led_hold_for_sleep(), the pad is still
    // latched from before this deep sleep -- ESP32 keeps an RTC GPIO hold
    // active through sleep AND after waking, specifically so the pin
    // doesn't glitch. gpio_config()/gpio_set_level() below have no effect
    // on a held pin, so release it first; led_hold_for_sleep() re-enables
    // the hold right before the next sleep.
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

void led_update(uint16_t distance_mm) {
    bool low_or_empty = (distance_mm >= LOW_STOCK_THRESHOLD_MM);
    gpio_set_level(LED_GPIO_PIN, low_or_empty ? 1 : 0);
}

void led_hold_for_sleep(void) {
    // Without this, the GPIO driver resets the pin to its default
    // (floating) state as soon as deep sleep begins, so the LED would
    // drop out every cycle regardless of what led_update() last set --
    // gpio_hold_en() latches the pad's current level through sleep, and
    // gpio_deep_sleep_hold_en() is the chip-wide switch that makes deep
    // sleep respect per-pin holds at all (off by default).
    gpio_hold_en(LED_GPIO_PIN);
    gpio_deep_sleep_hold_en();
}
