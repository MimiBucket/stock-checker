idf_component_register(
    SRCS "espnow_comm.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif log nvs_flash
)