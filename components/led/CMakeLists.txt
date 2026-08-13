idf_component_register(
    SRCS "led.cpp"
    INCLUDE_DIRS "include"
    REQUIRES log driver esp_driver_gpio
)