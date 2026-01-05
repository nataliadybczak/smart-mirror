#include "bme280.h"
#include <math.h>

// Struktura na dane kalibracyjne (pobrane z Twojego czujnika)
struct {
    uint16_t dig_T1; int16_t dig_T2, dig_T3;
    uint16_t dig_P1; int16_t dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
    uint8_t  dig_H1; int16_t dig_H2; uint8_t  dig_H3; int16_t dig_H4, dig_H5; int8_t  dig_H6;
    int32_t t_fine;
} cal;

esp_err_t bme280_init(i2c_port_t port, uint8_t addr) {
    uint8_t calib[26], calib2[7], cmd;
    // Czytanie parametrów kalibracji
    cmd = 0x88;
    i2c_master_write_read_device(port, addr, &cmd, 1, calib, 26, 1000/portTICK_PERIOD_MS);
    cal.dig_T1 = (calib[1] << 8) | calib[0]; cal.dig_T2 = (calib[3] << 8) | calib[2]; cal.dig_T3 = (calib[5] << 8) | calib[4];
    
    // Ustawienie trybu pracy (Normal mode)
    uint8_t config[2] = {0xF4, 0x27}; // Osamp x1, Mode Normal
    return i2c_master_write_to_device(port, addr, config, 2, 1000/portTICK_PERIOD_MS);
}

esp_err_t bme280_read_float_data(i2c_port_t port, uint8_t addr, float *temp, float *press, float *hum) {
    uint8_t cmd = 0xF7, d[8];
    i2c_master_write_read_device(port, addr, &cmd, 1, d, 8, 1000/portTICK_PERIOD_MS);
    
    int32_t adc_T = (d[3] << 12) | (d[4] << 4) | (d[5] >> 4);
    int32_t var1 = ((((adc_T>>3) - ((int32_t)cal.dig_T1<<1))) * ((int32_t)cal.dig_T2)) >> 11;
    int32_t var2 = (((((adc_T>>4) - ((int32_t)cal.dig_T1)) * ((adc_T>>4) - ((int32_t)cal.dig_T1))) >> 12) * ((int32_t)cal.dig_T3)) >> 14;
    cal.t_fine = var1 + var2;
    *temp = (cal.t_fine * 5 + 128) >> 8;
    *temp /= 100.0f;
    
    *press = 101325.0f; // Uproszczone dla testu
    *hum = 45.0f;       // Uproszczone dla testu
    return ESP_OK;
}