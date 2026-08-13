idf_component_register(
    SRCS "espnow_comm.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif freertos espnow_comm
)