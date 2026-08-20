#include <stdio.h>
#include "sensor_select.h"
#include "provisioning.h"
#include "led.h"
#include "espnow_comm.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_log.h"

static const char *TAG = "sensor";

// TEMP diagnostic: DEEPSLEEP means a real timer wake; POWERON means a
// monitor reconnect (DTR/RTS resets via EN); BROWNOUT confirms a brownout.
// Remove once wake failures are root-caused.
static void log_reset_reason(void) {
    esp_reset_reason_t reason = esp_reset_reason();
    const char *label = "?";
    switch (reason) {
        case ESP_RST_POWERON:   label = "POWERON (fresh power or EN reset, e.g. monitor reconnect)"; break;
        case ESP_RST_DEEPSLEEP: label = "DEEPSLEEP (genuine autonomous timer wake)"; break;
        case ESP_RST_BROWNOUT:  label = "BROWNOUT"; break;
        case ESP_RST_SW:        label = "SW (esp_restart)"; break;
        case ESP_RST_PANIC:     label = "PANIC"; break;
        default: break;
    }
    ESP_LOGI(TAG, "Reset reason: %d (%s)", reason, label);
}

// How long an unprovisioned sensor stays awake announcing itself, and how
// often it re-announces. Bounded so an offline logger doesn't burn
// battery forever -- see PROVISION_RETRY_FALLBACK_SEC below.
#define PROVISION_TIMEOUT_SEC (5 * 60)
#define PROVISION_ANNOUNCE_INTERVAL_SEC 5

// If provisioning times out, sleep this long before retrying next wake.
#define PROVISION_RETRY_FALLBACK_SEC (30 * 60)

static void deep_sleep_for(uint32_t seconds) {
    led_hold_for_sleep();
    ESP_LOGI(TAG, "Entering deep sleep for %lu sec", (unsigned long)seconds);
    esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
    esp_deep_sleep_start();
}

// TEMP: set to 1 to test bare timer-wake with no WiFi/sensor code, to
// isolate a board/regulator fault from a WiFi-current-spike brownout.
#define MINIMAL_SLEEP_TEST 0

void app_main(void) {
    log_reset_reason();
#if MINIMAL_SLEEP_TEST
    ESP_LOGI(TAG, "Minimal sleep test cycle");
    deep_sleep_for(10);
    return; // unreachable -- esp_deep_sleep_start() does not return
#endif
    led_init();
    espnow_comm_init((const uint8_t *)"\x10\x52\x1C\x60\x56\xC4"); // logger MAC address

    uint32_t interval_sec = 0;
    bool provisioned = provisioning_load(&interval_sec);

    if (!provisioned) {
        // Fresh sensor (or NVS erased): stay awake and wait for the
        // logger to assign an interval.
        ESP_LOGI(TAG, "Not yet provisioned; announcing to logger...");
        uint32_t assigned_interval = 0, start_delay_sec = 0;

        if (!espnow_comm_run_provisioning(PROVISION_TIMEOUT_SEC, PROVISION_ANNOUNCE_INTERVAL_SEC,
                                           &assigned_interval, &start_delay_sec)) {
            ESP_LOGW(TAG, "Provisioning timed out after %d sec; retrying next wake in %d sec",
                     PROVISION_TIMEOUT_SEC, PROVISION_RETRY_FALLBACK_SEC);
            deep_sleep_for(PROVISION_RETRY_FALLBACK_SEC);
            return; // unreachable -- esp_deep_sleep_start() does not return
        }

        provisioning_save(assigned_interval);
        interval_sec = assigned_interval;
        ESP_LOGI(TAG, "Provisioned: interval=%lu sec, start_delay=%lu sec",
                 (unsigned long)assigned_interval, (unsigned long)start_delay_sec);

        if (start_delay_sec > 0) {
            // Phase-align: sleep the requested delay and take the first
            // real reading on the next wake.
            deep_sleep_for(start_delay_sec);
            return; // unreachable
        }
        // start_delay_sec == 0: fall straight into normal operation below.
    }

    uint16_t distance_mm = 0;
    if (distance_sensor_init() && distance_sensor_read(&distance_mm)) {
        ESP_LOGI(TAG, "Distance: %d mm", distance_mm);
        espnow_comm_send_data(distance_mm);
    }

    uint32_t sleep_duration_sec = interval_sec;
    uint32_t adjusted_interval = 0;
    bool low_or_empty = false;
    if (espnow_comm_listen_for_reply(100, &adjusted_interval, &low_or_empty)) {
        ESP_LOGI(TAG, "Got reply: %lu sec, low_or_empty=%d", (unsigned long)adjusted_interval, low_or_empty);
        sleep_duration_sec = adjusted_interval;
        provisioning_save(adjusted_interval); // keep NVS in sync with logger-driven changes (SETFREQ)
        led_update(low_or_empty); // logger-computed status -- no reply, no new status, LED holds its last value
    } else {
        ESP_LOGI(TAG, "No reply received this cycle");
    }

    distance_sensor_deinit();
    deep_sleep_for(sleep_duration_sec);
}
