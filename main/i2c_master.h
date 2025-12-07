#pragma once
#include <stdint.h>
#include "esp_err.h"

esp_err_t i2c_master_init(void);
esp_err_t i2c_write_reg(uint8_t dev, uint8_t reg, uint8_t data);
esp_err_t i2c_read_regs(uint8_t dev, uint8_t start_reg, uint8_t *buf, size_t len);
