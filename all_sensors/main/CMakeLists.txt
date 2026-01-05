idf_component_register(SRCS "main.c" "mqtt_handler.c"
                    INCLUDE_DIRS "."
                    REQUIRES ssd1306 driver esp_driver_i2c bme280 esp_wifi esp_event mqtt esp_netif json nvs_flash)