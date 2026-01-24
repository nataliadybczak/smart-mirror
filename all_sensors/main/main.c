#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "driver/i2c.h"
#include "driver/uart.h"
#include "ssd1306.h"
#include "mqtt_handler.h"
#include "wifi_ble_manager.h"

#define TXD_PIN 17
#define RXD_PIN 16
#define I2C_PORT_NUM I2C_NUM_0

// Globalna zmienna ekranu (używana przez mqtt_handler.c jako extern)
SSD1306_t dev; 

// Funkcja wysyłająca komendę do DFPlayer
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
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(UART_NUM_2, &uart_config);
    uart_set_pin(UART_NUM_2, TXD_PIN, RXD_PIN, -1, -1);
    uart_driver_install(UART_NUM_2, 1024, 0, 0, NULL, 0);
}

void app_main(void) {
    // 1. Inicjalizacja UART (DFPlayer) - to zostaje, bo jest niezależne
    init_uart();
    vTaskDelay(pdMS_TO_TICKS(100));

    // 2. Inicjalizacja I2C i OLED - to zostaje
    i2c_driver_delete(I2C_PORT_NUM); 
    i2c_master_init(&dev, 21, 22, -1); 
    dev._address = 0x3C; 
    ssd1306_init(&dev, 128, 64);
    ssd1306_clear_screen(&dev, false);
    ssd1306_display_text(&dev, 2, "START SYSTEMU...", 14, false);

    // 3. Start Tasków Smart Mirror (OLED, Czujniki)
    // To przygotuje zmienne i taski, ale NIE włączy WiFi
    start_mqtt_handler();

    // 4. URUCHOMIENIE NOWEGO MODUŁU SIECIOWEGO (NVS + WiFi + BLE)
    // Ta funkcja zajmie się inicjalizacją NVS, WiFi i po połączeniu odpali MQTT
    wifi_ble_init(); 

    // Pętla główna
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}