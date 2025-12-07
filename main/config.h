// config.h
#pragma once

/* I2C config */
#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_MASTER_SDA_IO 21
#define I2C_MASTER_SCL_IO 22
#define I2C_MASTER_FREQ_HZ 400000
#define I2C_ACK_CHECK_EN 1
#define I2C_TIMEOUT_MS 1000

/* Device addresses */
#define BME280_ADDR1 0x76
#define BME280_ADDR2 0x77
#define SSD1306_ADDR 0x3C

/* BME280 registers */
#define BME280_REG_ID 0xD0
#define BME280_RESET 0xE0
#define BME280_CTRL_HUM 0xF2
#define BME280_STATUS 0xF3
#define BME280_CTRL_MEAS 0xF4
#define BME280_CONFIG 0xF5
#define BME280_PRESS_MSB 0xF7
#define BME280_CALIB00 0x88
#define BME280_CALIB26 0xE1

/* SSD1306 commands */
#define SSD1306_CONTROL_CMD 0x00
#define SSD1306_CONTROL_DATA 0x40
