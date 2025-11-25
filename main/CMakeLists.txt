idf_component_register(SRCS "ble_hid_sniffer.c" "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash bt esp_wifi esp_http_client esp_driver_gpio)
