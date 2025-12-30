set(srcs
    "ssd1306.c"
    "ssd1306_i2c_legacy.c"
    "ssd1306_spi.c" # Dodajemy to, żeby linker nie płakał
    )

idf_component_register(SRCS ${srcs}
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_driver_i2c esp_driver_spi)