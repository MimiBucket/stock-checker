idf_component_register(
    SRCS "src/ultrasonic_sensor.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer log
    PRIV_REQUIRES esp_driver_gpio
)
