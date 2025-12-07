#include "i2c_master.h"
#include "config.h"
#include "driver/i2c.h"

#define I2C_MASTER_SCL_IO 22
#define I2C_MASTER_SDA_IO 21
#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_MASTER_FREQ_HZ 400000

esp_err_t i2c_master_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_MASTER_NUM, &conf));
    return i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
}

esp_err_t i2c_write_reg(uint8_t dev, uint8_t reg, uint8_t data)
{
    return i2c_master_write_to_device(I2C_MASTER_NUM, dev, (uint8_t[]){reg, data}, 2, 1000 / portTICK_PERIOD_MS);
}

esp_err_t i2c_read_regs(uint8_t dev, uint8_t start_reg, uint8_t *buf, size_t len)
{
    return i2c_master_write_read_device(I2C_MASTER_NUM, dev, &start_reg, 1, buf, len, 1000 / portTICK_PERIOD_MS);
}
