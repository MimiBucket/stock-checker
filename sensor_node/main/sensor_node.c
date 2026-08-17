#include <stdio.h>
#include "tof_sensor.h"
#include "led.h"
#include "espnow_comm.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "sensor_node";

// How long a not-yet-provisioned sensor stays awake announcing itself
// and waiting for the logger to assign it an interval, and how often it
// re-announces during that window. Bounded on purpose: if the logger/PC
// is offline for a while, we don't want to sit here burning battery
// forever -- see PROVISION_RETRY_FALLBACK_SEC below.
#define PROVISION_TIMEOUT_SEC (5 * 60)
#define PROVISION_ANNOUNCE_INTERVAL_SEC 5

// If provisioning times out, sleep this long before trying again on the
// next wake, instead of retrying immediately or staying awake.
#define PROVISION_RETRY_FALLBACK_SEC (30 * 60)

#define NVS_NAMESPACE "prov"
#define NVS_KEY_INTERVAL "interval_sec"

// Reads the interval this sensor was last assigned, if any. Returns
// false if it's never been provisioned (namespace/key doesn't exist yet).
static bool provisioning_load(uint32_t *interval_sec_out) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_get_u32(handle, NVS_KEY_INTERVAL, interval_sec_out);
    nvs_close(handle);
    return err == ESP_OK;
}

// Persists the current interval so it survives a full power loss (deep
// sleep itself doesn't touch NVS, so this matters only for that case,
// plus keeping the very first provisioning result around at all).
static void provisioning_save(uint32_t interval_sec) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing provisioning state");
        return;
    }
    nvs_set_u32(handle, NVS_KEY_INTERVAL, interval_sec);
    nvs_commit(handle);
    nvs_close(handle);
}

static void deep_sleep_for(uint32_t seconds) {
    led_hold_for_sleep();
    ESP_LOGI(TAG, "Entering deep sleep for %lu sec", (unsigned long)seconds);
    esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
    esp_deep_sleep_start();
}

void app_main(void) {
    led_init();
    espnow_comm_init((const uint8_t *)"\x10\x52\x1C\x60\x56\xC4"); // logger MAC address

    uint32_t interval_sec = 0;
    bool provisioned = provisioning_load(&interval_sec);

    if (!provisioned) {
        // Fresh sensor (or NVS was erased): stay awake, radio on, and
        // wait for the logger to assign an interval -- easy to observe
        // happening live instead of guessing whether it's stuck asleep.
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
            // Phase-align: skip taking a reading this cycle, sleep the
            // requested delay, and take the first real reading next wake
            // (which will find `provisioned` true and fall into the
            // normal path below).
            deep_sleep_for(start_delay_sec);
            return; // unreachable
        }
        // start_delay_sec == 0 (the common case): fall straight into
        // normal operation below and take a reading immediately.
    }

    uint16_t distance_mm = 0;
    if (tof_sensor_init() && tof_sensor_read(&distance_mm)) {
        ESP_LOGI(TAG, "Distance: %d mm", distance_mm);
        led_update(distance_mm);
        espnow_comm_send_data(distance_mm);
    }

    uint32_t sleep_duration_sec = interval_sec;
    uint32_t adjusted_interval = 0;
    if (espnow_comm_listen_for_correction(100, &adjusted_interval)) {
        ESP_LOGI(TAG, "Got correction: %lu sec", (unsigned long)adjusted_interval);
        sleep_duration_sec = adjusted_interval;
        provisioning_save(adjusted_interval); // keep NVS in sync with logger-driven changes (SETFREQ)
    } else {
        ESP_LOGI(TAG, "No correction received this cycle");
    }

    tof_sensor_deinit();
    deep_sleep_for(sleep_duration_sec);
}
