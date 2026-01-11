#include "bme280.h"
#include <math.h>
#include "esp_log.h"

// Rozszerzona struktura kalibracji
struct {
    uint16_t dig_T1; int16_t dig_T2, dig_T3;
    uint16_t dig_P1; int16_t dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
    uint8_t  dig_H1; int16_t dig_H2; uint8_t  dig_H3; int16_t dig_H4, dig_H5; int8_t  dig_H6;
    int32_t t_fine;
} cal;

esp_err_t bme280_init(i2c_port_t port, uint8_t addr) {
    uint8_t calib[26], calib_h[7], cmd;
    
    // 1. Czytanie parametrów T1-T3 i P1-P9 (Adresy 0x88 do 0xA1)
    cmd = 0x88;
    i2c_master_write_read_device(port, addr, &cmd, 1, calib, 26, pdMS_TO_TICKS(100));
    
    cal.dig_T1 = (calib[1] << 8) | calib[0];
    cal.dig_T2 = (calib[3] << 8) | calib[2];
    cal.dig_T3 = (calib[5] << 8) | calib[4];

    cal.dig_P1 = (calib[7] << 8) | calib[6];
    cal.dig_P2 = (calib[9] << 8) | calib[8];
    cal.dig_P3 = (calib[11] << 8) | calib[10];
    cal.dig_P4 = (calib[13] << 8) | calib[12];
    cal.dig_P5 = (calib[15] << 8) | calib[14];
    cal.dig_P6 = (calib[17] << 8) | calib[16];
    cal.dig_P7 = (calib[19] << 8) | calib[18];
    cal.dig_P8 = (calib[21] << 8) | calib[20];
    cal.dig_P9 = (calib[23] << 8) | calib[22];

    // 2. Czytanie parametru H1 (Adres 0xA1)
    cmd = 0xA1;
    i2c_master_write_read_device(port, addr, &cmd, 1, &cal.dig_H1, 1, pdMS_TO_TICKS(100));

    // 3. Czytanie parametrów H2-H6 (Adresy 0xE1 do 0xE7)
    cmd = 0xE1;
    i2c_master_write_read_device(port, addr, &cmd, 1, calib_h, 7, pdMS_TO_TICKS(100));

    cal.dig_H2 = (calib_h[1] << 8) | calib_h[0];
    cal.dig_H3 = calib_h[2];
    cal.dig_H4 = (calib_h[3] << 4) | (calib_h[4] & 0x0F);
    cal.dig_H5 = (calib_h[5] << 4) | (calib_h[4] >> 4);
    cal.dig_H6 = (int8_t)calib_h[6];

    // 4. Konfiguracja: Musimy najpierw ustawić Humidity (0xF2), potem resztę (0xF4)
    uint8_t hum_cfg[2] = {0xF2, 0x01}; // Humidity oversampling x1
    i2c_master_write_to_device(port, addr, hum_cfg, 2, pdMS_TO_TICKS(100));

    uint8_t meas_cfg[2] = {0xF4, 0x27}; // Temp/Press oversampling x1, Mode Normal
    return i2c_master_write_to_device(port, addr, meas_cfg, 2, pdMS_TO_TICKS(100));
}

esp_err_t bme280_read_float_data(i2c_port_t port, uint8_t addr, float *temp, float *press, float *hum) {
    uint8_t cmd = 0xF7, d[8];
    esp_err_t ret = i2c_master_write_read_device(port, addr, &cmd, 1, d, 8, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) return ret;

    // Surowe dane z sensorów
    int32_t adc_P = (d[0] << 12) | (d[1] << 4) | (d[2] >> 4);
    int32_t adc_T = (d[3] << 12) | (d[4] << 4) | (d[5] >> 4);
    int32_t adc_H = (d[6] << 8) | d[7];

    // Kompensacja temperatury (wymagana do reszty obliczeń)
    int32_t var1_t = ((((adc_T >> 3) - ((int32_t)cal.dig_T1 << 1))) * ((int32_t)cal.dig_T2)) >> 11;
    int32_t var2_t = (((((adc_T >> 4) - ((int32_t)cal.dig_T1)) * ((adc_T >> 4) - ((int32_t)cal.dig_T1))) >> 12) * ((int32_t)cal.dig_T3)) >> 14;
    cal.t_fine = var1_t + var2_t;
    *temp = (float)((cal.t_fine * 5 + 128) >> 8) / 100.0f;

    // Kompensacja ciśnienia
    int64_t var1_p = ((int64_t)cal.t_fine) - 128000;
    int64_t var2_p = var1_p * var1_p * (int64_t)cal.dig_P6;
    var2_p = var2_p + ((var1_p * (int64_t)cal.dig_P5) << 17);
    var2_p = var2_p + (((int64_t)cal.dig_P4) << 35);
    var1_p = ((var1_p * var1_p * (int64_t)cal.dig_P3) >> 8) + ((var1_p * (int64_t)cal.dig_P2) << 12);
    var1_p = (((((int64_t)1) << 47) + var1_p)) * ((int64_t)cal.dig_P1) >> 33;

    if (var1_p == 0) {
        *press = 0;
    } else {
        int64_t p = 1048576 - adc_P;
        p = (((p << 31) - var2_p) * 3125) / var1_p;
        var1_p = (((int64_t)cal.dig_P9) * (p >> 13) * (p >> 13)) >> 25;
        var2_p = (((int64_t)cal.dig_P8) * p) >> 19;
        p = ((p + var1_p + var2_p) >> 8) + (((int64_t)cal.dig_P7) << 4);
        *press = (float)p / 256.0f / 100.0f; // Wynik w hPa
    }

    // Kompensacja wilgotności
    int32_t v_x1_u32r = (cal.t_fine - ((int32_t)76800));
    v_x1_u32r = (((((adc_H << 14) - (((int32_t)cal.dig_H4) << 20) - (((int32_t)cal.dig_H5) * v_x1_u32r)) +
                   ((int32_t)16384)) >> 15) * (((((((v_x1_u32r * ((int32_t)cal.dig_H6)) >> 10) *
                   (((v_x1_u32r * ((int32_t)cal.dig_H3)) >> 11) + ((int32_t)32768))) >> 10) + ((int32_t)2097152)) *
                   ((int32_t)cal.dig_H2) + 8192) >> 14));
    v_x1_u32r = (v_x1_u32r - (((((v_x1_u32r >> 15) * (v_x1_u32r >> 15)) >> 7) * ((int32_t)cal.dig_H1)) >> 4));
    v_x1_u32r = (v_x1_u32r < 0 ? 0 : v_x1_u32r);
    v_x1_u32r = (v_x1_u32r > 419430400 ? 419430400 : v_x1_u32r);
    *hum = (float)(v_x1_u32r >> 12) / 1024.0f;

    return ESP_OK;
}