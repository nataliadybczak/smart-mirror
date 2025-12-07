#pragma once
#include <stdint.h>

void ssd1306_init(void);
void ssd1306_clear(void);
void ssd1306_write_char(char c, uint8_t page, uint8_t col);
void ssd1306_write_string(const char* str, uint8_t page);
