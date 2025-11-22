#ifndef SSD1306_H
#define SSD1306_H

void ssd1306_init(void);
void ssd1306_clear_screen(void);
// Uprościliśmy funkcję, żeby przyjmowała tylko tekst (bez stron/segmentów, bo to symulacja)
void ssd1306_display_text(const char *text);

#endif