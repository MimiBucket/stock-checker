#include "provisioning.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "provisioning";

#define NVS_NAMESPACE "prov"
#define NVS_KEY_INTERVAL "interval_sec"

bool provisioning_load(uint32_t *interval_sec_out)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_get_u32(handle, NVS_KEY_INTERVAL, interval_sec_out);
    nvs_close(handle);
    return err == ESP_OK;
}

void provisioning_save(uint32_t interval_sec)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing provisioning state");
        return;
    }
    nvs_set_u32(handle, NVS_KEY_INTERVAL, interval_sec);
    nvs_commit(handle);
    nvs_close(handle);
}
