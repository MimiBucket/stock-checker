idf_component_register(
    SRCS "sensor_node.c" "provisioning.c"
    INCLUDE_DIRS "."
    REQUIRES espnow_comm led tof_sensor ultrasonic_sensor nvs_flash
)
