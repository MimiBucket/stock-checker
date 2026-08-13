idf_component_register(
    SRCS "pc_comm.c"
    INCLUDE_DIRS "include"
    REQUIRES espnow_comm freertos log
    PRIV_REQUIRES esp_driver_uart
)