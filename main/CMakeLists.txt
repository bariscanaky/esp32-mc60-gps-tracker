idf_component_register(SRCS "mc60.c" "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_uart esp_driver_gpio esp_timer)
