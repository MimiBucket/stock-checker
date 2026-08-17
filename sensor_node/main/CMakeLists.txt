idf_component_register(
    SRCS "sensor_node.c"
    INCLUDE_DIRS "."
    REQUIRES espnow_comm led tof_sensor nvs_flash
)
