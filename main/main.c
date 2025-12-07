// main.c
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "esp_err.h"
#include "sdkconfig.h"

static const char *TAG = "i2c_demo";

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
#define BME280_CTRL_HUM 0xF2
#define BME280_CTRL_MEAS 0xF4
#define BME280_CONFIG 0xF5
#define BME280_PRESS_MSB 0xF7
#define BME280_CALIB00 0x88
#define BME280_CALIB26 0xE1

/* SSD1306 commands */
#define SSD1306_CONTROL_CMD 0x00
#define SSD1306_CONTROL_DATA 0x40

/* Minimal font 8x8 */
static const uint8_t font8x8_basic[][8] = {
    {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00}, //0
    {0x18,0x38,0x18,0x18,0x18,0x18,0x3C,0x00}, //1
    {0x3C,0x66,0x06,0x0C,0x30,0x60,0x7E,0x00}, //2
    {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00}, //3
    {0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00}, //4
    {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00}, //5
    {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00}, //6
    {0x7E,0x66,0x06,0x0C,0x18,0x18,0x18,0x00}, //7
    {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}, //8
    {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00}, //9
    {0x7E,0x5A,0x18,0x18,0x18,0x18,0x18,0x00}, //T
    {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, //H
    {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00}, //C
    {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}, //P
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x00,0x00}, //h
    {0x00,0x3C,0x06,0x3E,0x66,0x3E,0x00,0x00}, //a
    {0x62,0x64,0x08,0x10,0x26,0x46,0x00,0x00}, //%
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, //:
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, //.
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}  //space
};

static int8_t char_to_index(char c){
    if(c>='0' && c<='9') return c-'0';
    if(c=='T') return 10;
    if(c=='H') return 11;
    if(c=='C') return 12;
    if(c=='P') return 13;
    if(c=='h') return 14;
    if(c=='a') return 15;
    if(c=='%') return 16;
    if(c==':') return 17;
    if(c=='.') return 18;
    if(c==' ') return 19;
    return 19;
}

/* I2C helpers */
static esp_err_t i2c_master_init(void){
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ
    };
    esp_err_t ret=i2c_param_config(I2C_MASTER_NUM,&conf);
    if(ret!=ESP_OK) return ret;
    return i2c_driver_install(I2C_MASTER_NUM, conf.mode,0,0,0);
}

static esp_err_t i2c_write_reg(uint8_t dev, uint8_t reg, uint8_t data){
    i2c_cmd_handle_t cmd=i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd,(dev<<1)|I2C_MASTER_WRITE,I2C_ACK_CHECK_EN);
    i2c_master_write_byte(cmd,reg,I2C_ACK_CHECK_EN);
    i2c_master_write_byte(cmd,data,I2C_ACK_CHECK_EN);
    i2c_master_stop(cmd);
    esp_err_t r=i2c_master_cmd_begin(I2C_MASTER_NUM,cmd,pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return r;
}

static esp_err_t i2c_read_regs(uint8_t dev, uint8_t reg, uint8_t* data, size_t len){
    if(len==0) return ESP_ERR_INVALID_ARG;
    i2c_cmd_handle_t cmd=i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd,(dev<<1)|I2C_MASTER_WRITE,I2C_ACK_CHECK_EN);
    i2c_master_write_byte(cmd,reg,I2C_ACK_CHECK_EN);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd,(dev<<1)|I2C_MASTER_READ,I2C_ACK_CHECK_EN);
    if(len>1) i2c_master_read(cmd,data,len-1,I2C_MASTER_ACK);
    i2c_master_read_byte(cmd,data+len-1,I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t r=i2c_master_cmd_begin(I2C_MASTER_NUM,cmd,pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return r;
}

/* SSD1306 */
static esp_err_t ssd1306_write_cmd(uint8_t cmd){
    i2c_cmd_handle_t h=i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h,(SSD1306_ADDR<<1)|I2C_MASTER_WRITE,I2C_ACK_CHECK_EN);
    uint8_t ctrl=SSD1306_CONTROL_CMD;
    i2c_master_write_byte(h,ctrl,I2C_ACK_CHECK_EN);
    i2c_master_write_byte(h,cmd,I2C_ACK_CHECK_EN);
    i2c_master_stop(h);
    esp_err_t r=i2c_master_cmd_begin(I2C_MASTER_NUM,h,pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(h);
    return r;
}

static esp_err_t ssd1306_init(void){
    const uint8_t seq[]={0xAE,0xD5,0x80,0xA8,0x3F,0xD3,0x00,0x40,0x8D,0x14,0x20,0x00,
                         0xA0,0xC0,0xDA,0x12,0x81,0xCF,0xD9,0xF1,0xDB,0x40,0xA4,0xA6,0xAF};
    for(size_t i=0;i<sizeof(seq);i++) ssd1306_write_cmd(seq[i]);
    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

static void ssd1306_clear(void){
    uint8_t zero[128*8]; memset(zero,0,sizeof(zero));
    for(int page=0; page<8; page++){
        ssd1306_write_cmd(0x21); ssd1306_write_cmd(0); ssd1306_write_cmd(127);
        ssd1306_write_cmd(0x22); ssd1306_write_cmd(page); ssd1306_write_cmd(page);
        i2c_cmd_handle_t h=i2c_cmd_link_create();
        i2c_master_start(h);
        i2c_master_write_byte(h,(SSD1306_ADDR<<1)|I2C_MASTER_WRITE,I2C_ACK_CHECK_EN);
        uint8_t ctrl=SSD1306_CONTROL_DATA;
        i2c_master_write_byte(h,ctrl,I2C_ACK_CHECK_EN);
        i2c_master_write(h, zero+128*page, 128, I2C_MASTER_ACK);
        i2c_master_stop(h);
        i2c_master_cmd_begin(I2C_MASTER_NUM,h,pdMS_TO_TICKS(I2C_TIMEOUT_MS));
        i2c_cmd_link_delete(h);
    }
}

static void ssd1306_write_string(const char* str, uint8_t page){
    uint8_t buf[128]; memset(buf,0,128);
    int col=0;
    for(const char* s=str; *s && col<16; s++, col++){
        int8_t idx=char_to_index(*s);
        for(int i=0;i<8;i++){
            if(col*8+i<128) buf[col*8+i]=font8x8_basic[idx][i];
        }
    }
    ssd1306_write_cmd(0x21); ssd1306_write_cmd(0); ssd1306_write_cmd(127);
    ssd1306_write_cmd(0x22); ssd1306_write_cmd(page); ssd1306_write_cmd(page);
    i2c_cmd_handle_t h=i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h,(SSD1306_ADDR<<1)|I2C_MASTER_WRITE,I2C_ACK_CHECK_EN);
    uint8_t ctrl=SSD1306_CONTROL_DATA;
    i2c_master_write_byte(h,ctrl,I2C_ACK_CHECK_EN);
    i2c_master_write(h,buf,128,I2C_MASTER_ACK);
    i2c_master_stop(h);
    i2c_master_cmd_begin(I2C_MASTER_NUM,h,pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(h);
}

/* BME280 minimal read/compensation */
typedef struct{
    uint16_t dig_T1; int16_t dig_T2,dig_T3;
    uint16_t dig_P1; int16_t dig_P2,dig_P3,dig_P4,dig_P5,dig_P6,dig_P7,dig_P8,dig_P9;
    uint8_t dig_H1; int16_t dig_H2; uint8_t dig_H3; int16_t dig_H4,dig_H5; int8_t dig_H6;
} bme280_calib_t;
static bme280_calib_t bme_calib; 
static int32_t t_fine;

static esp_err_t bme280_read_calib(uint8_t dev_addr)
{
    uint8_t buf1[26];
    esp_err_t err = i2c_read_regs(dev_addr, BME280_CALIB00, buf1, sizeof(buf1));
    if (err != ESP_OK) return err;

    /* parse calib (T and P) */
    bme_calib.dig_T1 = (uint16_t) (buf1[1] << 8) | buf1[0];
    bme_calib.dig_T2 = (int16_t)  (buf1[3] << 8) | buf1[2];
    bme_calib.dig_T3 = (int16_t)  (buf1[5] << 8) | buf1[4];
    bme_calib.dig_P1 = (uint16_t) (buf1[7] << 8) | buf1[6];
    bme_calib.dig_P2 = (int16_t)  (buf1[9] << 8) | buf1[8];
    bme_calib.dig_P3 = (int16_t)  (buf1[11] << 8) | buf1[10];
    bme_calib.dig_P4 = (int16_t)  (buf1[13] << 8) | buf1[12];
    bme_calib.dig_P5 = (int16_t)  (buf1[15] << 8) | buf1[14];
    bme_calib.dig_P6 = (int16_t)  (buf1[17] << 8) | buf1[16];
    bme_calib.dig_P7 = (int16_t)  (buf1[19] << 8) | buf1[18];
    bme_calib.dig_P8 = (int16_t)  (buf1[21] << 8) | buf1[20];
    bme_calib.dig_P9 = (int16_t)  (buf1[23] << 8) | buf1[22];
    bme_calib.dig_H1 = buf1[25];

    /* read rest for humidity */
    uint8_t buf2[7];
    err = i2c_read_regs(dev_addr, BME280_CALIB26, buf2, sizeof(buf2));
    if (err != ESP_OK) return err;

    bme_calib.dig_H2 = (int16_t) (buf2[1] << 8) | buf2[0];
    bme_calib.dig_H3 = buf2[2];
    bme_calib.dig_H4 = (int16_t) ((buf2[3] << 4) | (buf2[4] & 0x0F));
    bme_calib.dig_H5 = (int16_t) ((buf2[4] >> 4) | (buf2[5] << 4));
    bme_calib.dig_H6 = (int8_t) buf2[6];

    return ESP_OK;
}

static esp_err_t bme280_init(uint8_t dev_addr)
{
    uint8_t id;
    esp_err_t err = i2c_read_regs(dev_addr, BME280_REG_ID, &id, 1);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "BME280 ID: 0x%02x", id);
    // read calibration
    err = bme280_read_calib(dev_addr);
    if (err != ESP_OK) return err;
    // set oversampling humidity = 1
    err = i2c_write_reg(dev_addr, BME280_CTRL_HUM, 0x01); // osrs_h = 1
    if (err != ESP_OK) return err;
    // set ctrl_meas: temp oversample x1, press oversample x1, mode normal (0b00100111 = 0x27)? we'll use forced mode 0x25? Use normal: 0x27 = osrs_t=1 osrs_p=1 mode=normal
    err = i2c_write_reg(dev_addr, BME280_CTRL_MEAS, 0x27);
    if (err != ESP_OK) return err;
    // config: standby 1000ms, filter off
    err = i2c_write_reg(dev_addr, BME280_CONFIG, 0xA0); // t_sb=1000ms(101) <<5 -> (101<<5)=0xA0
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

static esp_err_t bme280_read_raw(uint8_t dev_addr, int32_t *adc_T, int32_t *adc_H, int32_t *adc_P)
{
    uint8_t data[8];
    esp_err_t err = i2c_read_regs(dev_addr, BME280_PRESS_MSB, data, 8); // read press(3), temp(3), hum(2)
    if (err != ESP_OK) return err;
    // temp
    *adc_T = (int32_t)((((uint32_t)data[3] << 12) | ((uint32_t)data[4] << 4) | ((data[5] >> 4) & 0x0F)));
    *adc_H = (int32_t)((data[6] << 8) | data[7]);
    return ESP_OK;
}

static int32_t bme280_compensate_T_int32(int32_t adc_T)
{
    // Follow the datasheet algorithm (integer)
    int32_t var1 = ((((adc_T >> 3) - ((int32_t)bme_calib.dig_T1 << 1))) * ((int32_t)bme_calib.dig_T2)) >> 11;
    int32_t var2 = (((((adc_T >> 4) - ((int32_t)bme_calib.dig_T1)) * ((adc_T >> 4) - ((int32_t)bme_calib.dig_T1))) >> 12) * ((int32_t)bme_calib.dig_T3)) >> 14;
    t_fine = var1 + var2;
    int32_t T = (t_fine * 5 + 128) >> 8;
    return T; // temperature in 0.01 degC
}

static int32_t bme280_compensate_H_int32(int32_t adc_H)
{
    int32_t v_x1_u32r;
    v_x1_u32r = (t_fine - ((int32_t)76800));
    v_x1_u32r = (((((adc_H << 14) - (((int32_t)bme_calib.dig_H4) << 20) - (((int32_t)bme_calib.dig_H5) * v_x1_u32r)) + ((int32_t)16384)) >> 15) *
                 (((((((v_x1_u32r * ((int32_t)bme_calib.dig_H6)) >> 10) * (((v_x1_u32r * ((int32_t)bme_calib.dig_H3)) >> 11) + ((int32_t)32768))) >> 10) + ((int32_t)2097152)) *
                   ((int32_t)bme_calib.dig_H2) + 8192) >> 14));
    v_x1_u32r = (v_x1_u32r - (((((v_x1_u32r >> 15) * (v_x1_u32r >> 15)) >> 7) * ((int32_t)bme_calib.dig_H1)) >> 4));
    v_x1_u32r = (v_x1_u32r < 0 ? 0 : v_x1_u32r);
    v_x1_u32r = (v_x1_u32r > 419430400 ? 419430400 : v_x1_u32r);
    return (v_x1_u32r >> 12); // returns value in Q22.10 format -> convert later
}

static int32_t bme280_compensate_P_int32(int32_t adc_P)
{
    int64_t var1, var2, p;
    var1 = ((int64_t)t_fine) - 128000;
    var2 = var1 * var1 * (int64_t)bme_calib.dig_P6;
    var2 = var2 + ((var1 * (int64_t)bme_calib.dig_P5) << 17);
    var2 = var2 + (((int64_t)bme_calib.dig_P4) << 35);
    var1 = ((var1 * var1 * (int64_t)bme_calib.dig_P3) >> 8) + ((var1 * (int64_t)bme_calib.dig_P2) << 12);
    var1 = (((((int64_t)1) << 47) + var1) * ((int64_t)bme_calib.dig_P1)) >> 33;
    if (var1 == 0) return 0; // avoid div by zero
    p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t)bme_calib.dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)bme_calib.dig_P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)bme_calib.dig_P7) << 4);
    return (int32_t)(p >> 8); // Pa
}

void app_main(void){
    ESP_LOGI(TAG,"Starting I2C + BME280 + SSD1306 demo");
    ESP_ERROR_CHECK(i2c_master_init());
    ESP_ERROR_CHECK(ssd1306_init());
    ssd1306_clear();

    uint8_t bme_addr=0, id;
    if(i2c_read_regs(BME280_ADDR1,BME280_REG_ID,&id,1)==ESP_OK) bme_addr=BME280_ADDR1;
    else if(i2c_read_regs(BME280_ADDR2,BME280_REG_ID,&id,1)==ESP_OK) bme_addr=BME280_ADDR2;

    if(bme_addr){
        ESP_LOGI(TAG,"Found BME280 at 0x%02x",bme_addr);
        bme280_init(bme_addr);
    }

    while(1){
        if(bme_addr){
            int32_t rawT, rawH, rawP;
            if(bme280_read_raw(bme_addr, &rawT, &rawH, &rawP) == ESP_OK){
                int32_t temp_x100 = bme280_compensate_T_int32(rawT);
                int32_t hum_q = bme280_compensate_H_int32(rawH);
                int32_t pres = bme280_compensate_P_int32(rawP);

                float temp = temp_x100 / 100.0f;
                float hum = hum_q / 1024.0f;
                float pres_hPa = pres / 100.0f;

                ESP_LOGI(TAG,"T=%.2f C, H=%.2f %%, P=%.2f hPa", temp, hum, pres_hPa);

                char line1[17], line2[17], line3[17];
                snprintf(line1, sizeof(line1), "T:%.1fC", temp);
                snprintf(line2, sizeof(line2), "H:%.1f%%", hum);
                snprintf(line3, sizeof(line3), "P:%.0fhPa", pres_hPa);

                ssd1306_clear();
                ssd1306_write_string(line1, 0);
                ssd1306_write_string(line2, 1);
                ssd1306_write_string(line3, 2);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
