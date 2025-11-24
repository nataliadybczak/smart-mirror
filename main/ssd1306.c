#include "ssd1306.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "VIRTUAL_OLED";

// Bufor pamięci ekranu (żebyśmy wiedzieli co "jest" wyświetlane)
static char virtual_screen_buffer[128] = "[PUSTY]";

void ssd1306_init(void) {
    // Tu normalnie konfigurowalibyśmy I2C.
    // W wersji wirtualnej tylko informujemy o sukcesie.
    ESP_LOGI(TAG, "------------------------------------------------");
    ESP_LOGI(TAG, " INICJALIZACJA EKRANU (TRYB SYMULACJI) ");
    ESP_LOGI(TAG, " Sterownik gotowy. Rozdzielczość 128x64.");
    ESP_LOGI(TAG, "------------------------------------------------");
}

void ssd1306_clear_screen(void) {
    // Czyścimy nasz wirtualny bufor
    snprintf(virtual_screen_buffer, sizeof(virtual_screen_buffer), "[PUSTY - EKRAN CZARNY]");
    // ESP_LOGI(TAG, "Ekran wyczyszczony (wygaszony).");
}

// Funkcja do symulacji wyświetlania tekstu
void ssd1306_display_text(const char *text) {
    // Zapisujemy w buforze
    snprintf(virtual_screen_buffer, sizeof(virtual_screen_buffer), "%s", text);

    // "Rysujemy" w logach ramkę, żeby to ładnie wyglądało
    ESP_LOGI(TAG, "╔════════════════════════════════════╗");
    ESP_LOGI(TAG, "║ OLED DISPLAY:                      ║");
    ESP_LOGI(TAG, "║ %-34s ║", text); // %-34s wyrównuje do lewej
    ESP_LOGI(TAG, "╚════════════════════════════════════╝");
}