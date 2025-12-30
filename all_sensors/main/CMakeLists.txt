idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES ssd1306 driver esp_driver_i2c)