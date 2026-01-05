#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "ssd1306.h"
#include "bme280.h"
#include "mqtt_handler.h"
#include "nvs_flash.h"

#define I2C_PORT I2C_NUM_0
#define PIR_PIN 27
#define BH1750_ADDR 0x23
#define BME280_ADDR 0x76 
#define TXD_PIN 17
#define RXD_PIN 16

void send_dfplayer_cmd(uint8_t cmd, uint16_t dat) {
    uint8_t msg[10] = {0x7E, 0xFF, 0x06, cmd, 0x00, (uint8_t)(dat >> 8), (uint8_t)(dat & 0xFF), 0x00, 0x00, 0xEF};
    uint16_t checksum = 0;
    for (int i = 1; i < 7; i++) checksum += msg[i];
    checksum = -checksum;
    msg[7] = (uint8_t)(checksum >> 8);
    msg[8] = (uint8_t)(checksum & 0xFF);
    uart_write_bytes(UART_NUM_2, (const char*)msg, 10);
}

void init_uart() {
    uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_param_config(UART_NUM_2, &uart_config);
    uart_set_pin(UART_NUM_2, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_NUM_2, 1024, 0, 0, NULL, 0);
}

SSD1306_t dev;

SSD1306_t dev;

void app_main(void) {
    // 1. Inicjalizacja pamięci (Wspólna dla wszystkich modułów)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    init_uart();
    vTaskDelay(pdMS_TO_TICKS(500));
    send_dfplayer_cmd(0x06, 20); 
    vTaskDelay(pdMS_TO_TICKS(100));

    // 1. OLED i I2C
    
    i2c_master_init(&dev, 21, 22, -1); 
    dev._address = 0x3C; 
    ssd1306_init(&dev, 128, 64);
    ssd1306_clear_screen(&dev, false);

    start_mqtt_handler();

    // 2. BME280 - TERAZ POPRAWIONE
    bme280_init(I2C_PORT, BME280_ADDR); 

    // 3. PIR i BH1750
    gpio_reset_pin(PIR_PIN);
    gpio_set_direction(PIR_PIN, GPIO_MODE_INPUT);
    uint8_t cmd_on = 0x01, cmd_meas = 0x10;
    i2c_master_write_to_device(I2C_PORT, BH1750_ADDR, &cmd_on, 1, 100);
    i2c_master_write_to_device(I2C_PORT, BH1750_ADDR, &cmd_meas, 1, 100);

    char buf_t[20], buf_l[20], buf_time[20];

    while (1) {
        // Czas
        time_t now;
        struct tm ti;
        time(&now);
        localtime_r(&now, &ti);
        strftime(buf_time, sizeof(buf_time), "%H:%M:%S", &ti);

        // Odczyty
        float temp = 0, hum = 0, press = 0, lux = 0;
        bme280_read_float_data(I2C_PORT, BME280_ADDR, &temp, &press, &hum);
        
        uint8_t d[2];
        if (i2c_master_read_from_device(I2C_PORT, BH1750_ADDR, d, 2, 100) == ESP_OK) {
            lux = ((d[0] << 8) | d[1]) / 1.2;
        }

        // Ekran
        ssd1306_clear_screen(&dev, false);
        ssd1306_display_text(&dev, 0, buf_time, strlen(buf_time), false);
        
        snprintf(buf_t, sizeof(buf_t), "T:%.1fC H:%.0f%%", temp, hum);
        ssd1306_display_text(&dev, 2, buf_t, strlen(buf_t), false);
        

        if (lux > 600.0) {
            ssd1306_display_text(&dev, 5, "JASNO - GRA!", 12, true);
            send_dfplayer_cmd(0x12, 1); 
            vTaskDelay(pdMS_TO_TICKS(10000));
            send_dfplayer_cmd(0x0E, 0);
        } else if (gpio_get_level(PIR_PIN)) {
            ssd1306_display_text(&dev, 6, "WIDZE CIE!", 10, true);
        } else {
            snprintf(buf_l, sizeof(buf_l), "Lux: %.1f", lux);
            ssd1306_display_text(&dev, 6, buf_l, strlen(buf_l), false);
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}