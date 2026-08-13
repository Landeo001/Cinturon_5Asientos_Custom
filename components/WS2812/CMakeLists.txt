idf_component_register(
    SRCS "ws2812.c"
    INCLUDE_DIRS "include"
    REQUIRES led_strip esp_driver_rmt driver
)