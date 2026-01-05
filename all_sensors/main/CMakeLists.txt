idf_component_register(SRCS "main.c" "mqtt_handler.c"
                    INCLUDE_DIRS "."
                    REQUIRES ssd1306 driver bme280 esp_wifi esp_event nvs_flash esp_netif mqtt json esp_timer)