#pragma once
#include <stdint.h>
#include "esp_err.h"

esp_err_t bme280_init(uint8_t addr);
esp_err_t bme280_read_raw(uint8_t dev_addr, int32_t *adc_T, int32_t *adc_H, int32_t *adc_P);
int32_t bme280_compensate_T_int32(int32_t adc_T);
int32_t bme280_compensate_H_int32(int32_t adc_H);
int32_t bme280_compensate_P_int32(int32_t adc_P);
uint8_t bme280_detect(void);
