idf_component_register(SRCS "mqtt_handler.c" "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES ssd1306 driver esp_driver_i2c bme280)