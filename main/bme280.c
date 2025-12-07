#include "bme280.h"
#include "i2c_master.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BME280";

/* ---------------------------------------------------------
 * AUTO-DETECT DEVICE ADDRESS (0x76 / 0x77)
 * ---------------------------------------------------------*/
uint8_t bme280_detect(void)
{
    uint8_t id;
    if (i2c_read_regs(0x76, BME280_REG_ID, &id, 1) == ESP_OK && id == 0x60) return 0x76;
    if (i2c_read_regs(0x77, BME280_REG_ID, &id, 1) == ESP_OK && id == 0x60) return 0x77;
    return 0;
}

/* ---------------------------------------------------------
 * CALIBRATION STRUCT + GLOBAL VARIABLES
 * ---------------------------------------------------------*/
typedef struct {
    uint16_t dig_T1; int16_t dig_T2, dig_T3;
    uint16_t dig_P1; int16_t dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
    uint8_t dig_H1; int16_t dig_H2; uint8_t dig_H3; int16_t dig_H4, dig_H5; int8_t dig_H6;
} bme280_calib_t;

static bme280_calib_t bme_calib;
static int32_t t_fine;

/* ---------------------------------------------------------
 * READ CALIBRATION DATA
 * ---------------------------------------------------------*/
static esp_err_t bme280_read_calib(uint8_t dev_addr)
{
    uint8_t buf1[26];
    esp_err_t err = i2c_read_regs(dev_addr, BME280_CALIB00, buf1, sizeof(buf1));
    if (err != ESP_OK) return err;

    bme_calib.dig_T1 = (uint16_t)(buf1[1] << 8 | buf1[0]);
    bme_calib.dig_T2 = (int16_t)(buf1[3] << 8 | buf1[2]);
    bme_calib.dig_T3 = (int16_t)(buf1[5] << 8 | buf1[4]);

    bme_calib.dig_P1 = (uint16_t)(buf1[7] << 8 | buf1[6]);
    bme_calib.dig_P2 = (int16_t)(buf1[9] << 8 | buf1[8]);
    bme_calib.dig_P3 = (int16_t)(buf1[11] << 8 | buf1[10]);
    bme_calib.dig_P4 = (int16_t)(buf1[13] << 8 | buf1[12]);
    bme_calib.dig_P5 = (int16_t)(buf1[15] << 8 | buf1[14]);
    bme_calib.dig_P6 = (int16_t)(buf1[17] << 8 | buf1[16]);
    bme_calib.dig_P7 = (int16_t)(buf1[19] << 8 | buf1[18]);
    bme_calib.dig_P8 = (int16_t)(buf1[21] << 8 | buf1[20]);
    bme_calib.dig_P9 = (int16_t)(buf1[23] << 8 | buf1[22]);
    bme_calib.dig_H1 = buf1[25];

    uint8_t buf2[7];
    err = i2c_read_regs(dev_addr, BME280_CALIB26, buf2, sizeof(buf2));
    if (err != ESP_OK) return err;

    bme_calib.dig_H2 = (int16_t)(buf2[1] << 8 | buf2[0]);
    bme_calib.dig_H3 = buf2[2];
    bme_calib.dig_H4 = (int16_t)((buf2[3] << 4) | (buf2[4] & 0x0F));
    bme_calib.dig_H5 = (int16_t)((buf2[4] >> 4) | (buf2[5] << 4));
    bme_calib.dig_H6 = (int8_t)buf2[6];

    return ESP_OK;
}

/* ---------------------------------------------------------
 * INIT SENSOR
 * ---------------------------------------------------------*/
esp_err_t bme280_init(uint8_t dev_addr)
{
    uint8_t id;
    esp_err_t err = i2c_read_regs(dev_addr, BME280_REG_ID, &id, 1);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "BME280 ID: 0x%02X", id);

    // Load calibration data
    err = bme280_read_calib(dev_addr);
    if (err != ESP_OK) return err;

    // Humidity oversampling x1
    err = i2c_write_reg(dev_addr, BME280_CTRL_HUM, 0x01);
    if (err != ESP_OK) return err;

    // Temp x1, Pressure x1, Normal mode
    err = i2c_write_reg(dev_addr, BME280_CTRL_MEAS, 0x27);
    if (err != ESP_OK) return err;

    // Standby 1000ms, Filter off
    err = i2c_write_reg(dev_addr, BME280_CONFIG, 0xA0);
    if (err != ESP_OK) return err;

    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

/* ---------------------------------------------------------
 * READ RAW DATA (PRESS/TEMP/HUM)
 * ---------------------------------------------------------*/
esp_err_t bme280_read_raw(uint8_t dev_addr, int32_t *adc_T, int32_t *adc_H, int32_t *adc_P)
{
    uint8_t data[8];
    esp_err_t err = i2c_read_regs(dev_addr, BME280_PRESS_MSB, data, 8); // odczyt press(3), temp(3), hum(2)
    if (err != ESP_OK) return err;

    *adc_P = (int32_t)(((uint32_t)data[0] << 12) | ((uint32_t)data[1] << 4) | ((data[2] >> 4) & 0x0F));
    *adc_T = (int32_t)(((uint32_t)data[3] << 12) | ((uint32_t)data[4] << 4) | ((data[5] >> 4) & 0x0F));
    *adc_H = (int32_t)((data[6] << 8) | data[7]);

    return ESP_OK;
}


/* ---------------------------------------------------------
 * TEMPERATURE COMPENSATION
 * ---------------------------------------------------------*/
int32_t bme280_compensate_T_int32(int32_t adc_T)
{
    int32_t var1 = ((((adc_T >> 3) - ((int32_t)bme_calib.dig_T1 << 1))) *
                    ((int32_t)bme_calib.dig_T2)) >> 11;

    int32_t var2 = (((((adc_T >> 4) - ((int32_t)bme_calib.dig_T1)) *
                      ((adc_T >> 4) - ((int32_t)bme_calib.dig_T1))) >> 12) *
                    ((int32_t)bme_calib.dig_T3)) >> 14;

    t_fine = var1 + var2;

    return (t_fine * 5 + 128) >> 8;   // °C × 100
}

/* ---------------------------------------------------------
 * HUMIDITY COMPENSATION
 * ---------------------------------------------------------*/
int32_t bme280_compensate_H_int32(int32_t adc_H)
{
    int32_t v_x1 = t_fine - 76800;

    v_x1 = (((((adc_H << 14) - (((int32_t)bme_calib.dig_H4) << 20) -
               ((int32_t)bme_calib.dig_H5 * v_x1)) + 16384) >> 15) *
            (((((((v_x1 * bme_calib.dig_H6) >> 10) *
                 (((v_x1 * bme_calib.dig_H3) >> 11) + 32768)) >> 10) + 2097152) *
               bme_calib.dig_H2 + 8192) >> 14));

    v_x1 = v_x1 - ((((v_x1 >> 15) * (v_x1 >> 15)) >> 7) * bme_calib.dig_H1 >> 4);

    if (v_x1 < 0) v_x1 = 0;
    if (v_x1 > 419430400) v_x1 = 419430400;

    return v_x1 >> 12; // Q22.10 → /1024 later
}
/* ---------------------------------------------------------
 * CIŚNIENIE COMPENSATION
 * ---------------------------------------------------------*/
int32_t bme280_compensate_P_int32(int32_t adc_P)
{
    int64_t var1, var2, p;
    var1 = ((int64_t)t_fine) - 128000;
    var2 = var1 * var1 * (int64_t)bme_calib.dig_P6;
    var2 = var2 + ((var1*(int64_t)bme_calib.dig_P5)<<17);
    var2 = var2 + (((int64_t)bme_calib.dig_P4)<<35);
    var1 = ((var1 * var1 * (int64_t)bme_calib.dig_P3)>>8) + ((var1 * (int64_t)bme_calib.dig_P2)<<12);
    var1 = (((((int64_t)1)<<47)+var1))*((int64_t)bme_calib.dig_P1)>>33;
    if (var1 == 0) return 0; // avoid division by zero
    p = 1048576 - adc_P;
    p = (((p<<31) - var2)*3125)/var1;
    var1 = (((int64_t)bme_calib.dig_P9) * (p>>13) * (p>>13)) >> 25;
    var2 = (((int64_t)bme_calib.dig_P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)bme_calib.dig_P7)<<4);
    return (int32_t)(p>>8); // w Pa
}
