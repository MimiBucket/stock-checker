idf_component_register(
    SRCS "src/tof_sensor.cpp" "src/vl53l1_platform.c" "src/VL53L1X_api.c" 
    INCLUDE_DIRS "include"
    REQUIRES driver freertos log
    PRIV_REQUIRES esp_driver_i2c
)