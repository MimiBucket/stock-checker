idf_component_register(
    SRCS "tof_sensor.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c espp__vl53l driver
)