#include "pc_comm.h"
#include "espnow_comm.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/time.h>

static const char *TAG = "pc_comm";

#define UART_PORT UART_NUM_0
#define UART_BUF_SIZE 128

// ESP-IDF's console (ESP_LOGx) is configured to the same UART0 that this
// file uses for the DATA/FREQ/SENSORS/PROVISIONING wire protocol -- there's
// only one physical UART exposed over USB, so both share it. Without this
// mutex, a log line from another task (e.g. espnow_process_task) can
// interleave mid-character with a protocol line being printed here,
// corrupting both into unreadable/unparseable garbage. console_vprintf()
// below routes ESP_LOGx through the same mutex so the two can only ever
// alternate whole lines, never interleave within one.
static SemaphoreHandle_t s_console_mutex;

static int console_vprintf(const char *fmt, va_list args) {
    xSemaphoreTake(s_console_mutex, portMAX_DELAY);
    int ret = vprintf(fmt, args);
    xSemaphoreGive(s_console_mutex);
    return ret;
}

// Use this instead of printf() for every protocol line sent to the PC, so
// it's serialized against ESP_LOGx output via the same mutex. Also echoes
// the line through ESP_LOGI (as "-> PC: ...") so it's visible in the
// monitor log, not just in whatever's consuming the raw protocol stream.
static void console_print_line(const char *fmt, ...) {
    char buf[400]; // largest caller is pc_comm_send_sensor_list's SENSORS line
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    xSemaphoreTake(s_console_mutex, portMAX_DELAY);
    fputs(buf, stdout);
    xSemaphoreGive(s_console_mutex);

    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') {
        buf[len - 1] = '\0'; // ESP_LOGI adds its own newline
    }
    ESP_LOGI(TAG, "-> PC: %s", buf);
}

void pc_comm_init(void) {
    s_console_mutex = xSemaphoreCreateMutex();
    esp_log_set_vprintf(console_vprintf);

    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(UART_PORT, &uart_config);
    uart_driver_install(UART_PORT, UART_BUF_SIZE * 2, 0, 0, NULL, 0);
}

void pc_comm_send_freq(const uint8_t mac[6], uint32_t interval_sec, time_t anchor_epoch) {
    console_print_line("FREQ %02x:%02x:%02x:%02x:%02x:%02x %u %lld\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], (unsigned)interval_sec, (long long)anchor_epoch);
}

void pc_comm_send_all_freq(void) {
    int count = espnow_comm_get_sensor_count();
    for (int i = 0; i < count; i++) {
        uint8_t mac[6];
        uint32_t interval_sec;
        time_t anchor_epoch;
        if (espnow_comm_get_sensor_info(i, mac, &interval_sec, &anchor_epoch)) {
            pc_comm_send_freq(mac, interval_sec, anchor_epoch);
        }
    }
}

void pc_comm_send_provisioning(const uint8_t mac[6]) {
    console_print_line("PROVISIONING %02x:%02x:%02x:%02x:%02x:%02x\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void pc_comm_send_sensor_list(const uint8_t sensor_macs[][6], int count) {
    char line[16 + 18 * 20]; // "SENSORS " + up to 20 "xx:xx:xx:xx:xx:xx," entries
    int offset = snprintf(line, sizeof(line), "SENSORS ");
    for (int i = 0; i < count && offset < (int)sizeof(line) - 1; i++) {
        offset += snprintf(line + offset, sizeof(line) - offset,
                            "%s%02x:%02x:%02x:%02x:%02x:%02x",
                            i == 0 ? "" : ",",
                            sensor_macs[i][0], sensor_macs[i][1], sensor_macs[i][2],
                            sensor_macs[i][3], sensor_macs[i][4], sensor_macs[i][5]);
    }
    snprintf(line + offset, sizeof(line) - offset, "\n");
    console_print_line("%s", line);
}

// Parses "xx:xx:xx:xx:xx:xx" into 6 raw bytes. Returns false (and leaves
// mac_out untouched) if str isn't a well-formed MAC string.
static bool parse_mac(const char *str, uint8_t mac_out[6]) {
    unsigned int b[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        mac_out[i] = (uint8_t)b[i];
    }
    return true;
}

// Reads whatever's currently waiting on UART and dispatches SETTIME/SETFREQ.
// Returns true specifically when a time sync was applied, since
// pc_comm_wait_for_initial_sync() uses that to know it can stop waiting.
static bool check_and_handle_pc_commands(void) {
    uint8_t data[UART_BUF_SIZE];
    int len = uart_read_bytes(UART_PORT, data, UART_BUF_SIZE - 1, 0);
    if (len <= 0) return false;
    data[len] = '\0';

    if (strncmp((char *)data, "SETTIME ", 8) == 0) {
        long epoch_sec = atol((char *)data + 8);
        if (epoch_sec > 0) {
            struct timeval tv = { .tv_sec = epoch_sec, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "Time set from PC: %ld", epoch_sec);
            return true;
        }
    } else if (strncmp((char *)data, "SETFREQ ", 8) == 0) {
        // "SETFREQ <ALL|mac> <interval_sec> [anchor_epoch]" -- anchor_epoch
        // is optional and defaults to 0 (plain wall-clock alignment, e.g.
        // every hour on the hour) when the PC doesn't care about phase-
        // locking to a specific start time.
        char target[32] = {0};
        long interval_sec = 0;
        long long anchor_epoch = 0;
        int n = sscanf((char *)data + 8, "%31s %ld %lld", target, &interval_sec, &anchor_epoch);
        if ((n == 2 || n == 3) && interval_sec > 0) {
            if (strcmp(target, "ALL") == 0) {
                espnow_comm_set_schedule(NULL, (uint32_t)interval_sec, (time_t)anchor_epoch);
                pc_comm_send_all_freq();
                ESP_LOGI(TAG, "Schedule set from PC for ALL sensors: interval=%ld anchor=%lld", interval_sec, anchor_epoch);
            } else {
                uint8_t mac[6];
                if (parse_mac(target, mac)) {
                    espnow_comm_set_schedule(mac, (uint32_t)interval_sec, (time_t)anchor_epoch);
                    pc_comm_send_freq(mac, (uint32_t)interval_sec, (time_t)anchor_epoch);
                    ESP_LOGI(TAG, "Schedule set from PC for %s: interval=%ld anchor=%lld", target, interval_sec, anchor_epoch);
                } else {
                    ESP_LOGW(TAG, "SETFREQ: bad target %s", target);
                }
            }
        }
    }
    return false;
}

void pc_comm_wait_for_initial_sync(void) {
    ESP_LOGI(TAG, "Waiting for time sync from PC...");
    while (!check_and_handle_pc_commands()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "Time synced.");
}

// How often to log a heartbeat when the task has otherwise gone quiet, so
// "no output" in the monitor log can be told apart from "board reset/hung".
#define HEARTBEAT_INTERVAL_MS 5000

static void pc_comm_task(void *arg) {
    uint8_t mac[6];
    uint16_t distance_mm;
    TickType_t last_heartbeat = xTaskGetTickCount();

    while (1) {
        check_and_handle_pc_commands(); // periodic resync + SETFREQ handling

        if (espnow_comm_get_latest_reading(mac, &distance_mm)) {
            char line[64];
            snprintf(line, sizeof(line), "DATA %02x:%02x:%02x:%02x:%02x:%02x %d\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], distance_mm);
            console_print_line("%s", line);
        }

        if (espnow_comm_get_latest_provisioning_event(mac)) {
            pc_comm_send_provisioning(mac);
        }

        TickType_t now = xTaskGetTickCount();
        if (now - last_heartbeat >= pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS)) {
            ESP_LOGI(TAG, "heartbeat: pc_comm_task alive, %d sensor(s) registered",
                     espnow_comm_get_sensor_count());
            last_heartbeat = now;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void pc_comm_start_task(void) {
    xTaskCreate(pc_comm_task, "pc_comm_task", 4096, NULL, 5, NULL);
}