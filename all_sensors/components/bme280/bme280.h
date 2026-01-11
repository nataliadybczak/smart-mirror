#ifndef BME280_H
#define BME280_H

#include "esp_err.h"    // Opcjonalnie, dla pewności typu esp_err_t
#include "driver/i2c.h"

esp_err_t bme280_init(i2c_port_t port, uint8_t addr);
esp_err_t bme280_read_float_data(i2c_port_t port, uint8_t addr, float *temp, float *press, float *hum);

#endif