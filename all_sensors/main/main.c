#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "ssd1306.h"

#define I2C_PORT I2C_NUM_0
#define PIR_PIN 27
#define BH1750_ADDR 0x23

void app_main(void) {
    // 1. INICJALIZACJA OLED (Biblioteka sama zainstaluje sterownik I2C)
    SSD1306_t dev;
    // Ta funkcja ustawi piny 21, 22 i zainstaluje driver I2C_NUM_0
    // Ostatni parametr -1 oznacza, że nie używamy pinu RESET
    i2c_master_init(&dev, 21, 22, -1); 
    
    // Ustawiamy adres (w tej bibliotece to pole nazywa się _address)
    dev._address = 0x3C; 
    
    ssd1306_init(&dev, 128, 64);
    ssd1306_clear_screen(&dev, false);
    ssd1306_contrast(&dev, 0xFF);

    // 2. INICJALIZACJA PIR
    gpio_reset_pin(PIR_PIN);
    gpio_set_direction(PIR_PIN, GPIO_MODE_INPUT);

    // 3. START BH1750 
    // Driver I2C jest już zainstalowany przez OLED, więc możemy wysyłać komendy
    uint8_t cmd_on = 0x01;
    uint8_t cmd_measure = 0x10;
    i2c_master_write_to_device(I2C_PORT, BH1750_ADDR, &cmd_on, 1, 100/portTICK_PERIOD_MS);
    i2c_master_write_to_device(I2C_PORT, BH1750_ADDR, &cmd_measure, 1, 100/portTICK_PERIOD_MS);

    ssd1306_display_text(&dev, 0, "LUSTERKO v1.5", 13, false);
    vTaskDelay(pdMS_TO_TICKS(1000));

    char buffer[20];
    while (1) {
        uint8_t data[2];
        float lux = 0;
        if (i2c_master_read_from_device(I2C_PORT, BH1750_ADDR, data, 2, 100/portTICK_PERIOD_MS) == ESP_OK) {
            lux = ((data[0] << 8) | data[1]) / 1.2;
        }

        int ruch = gpio_get_level(PIR_PIN);

        ssd1306_clear_screen(&dev, false);
        snprintf(buffer, sizeof(buffer), "Jasno: %.1f lx", lux);
        ssd1306_display_text(&dev, 2, buffer, strlen(buffer), false);

        if (ruch) {
            ssd1306_display_text(&dev, 4, "WIDZE CIE!", 10, true);
        } else {
            ssd1306_display_text(&dev, 4, "Brak ruchu", 10, false);
        }

        vTaskDelay(pdMS_TO_TICKS(300));
    }
}