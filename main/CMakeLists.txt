idf_component_register(SRCS "hello_world_main.c"
                    PRIV_REQUIRES spi_flash
                    INCLUDE_DIRS ""
                    REQUIRES driver nvs_flash bt esp_timer lis3dh dy_sv17f WS2812)
