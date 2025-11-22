#include <stdio.h>
#include "nvs_flash.h"
#include "esp_log.h"

// Dołączamy nagłówki naszych modułów
#include "legacy_wifiAP.h"
#include "mqtt_handler.h"

static const char *TAG = "MAIN_CONTROLLER";

void app_main(void)
{
    // 1. Inicjalizacja pamięci (Wspólna dla wszystkich modułów)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Start systemu...");

    //WYBÓR TRYBU PRACY

    // OPCJA A: Stary projekt (z AP i łajfaj)
    // start_legacy_counter();

    // OPCJA B: Nowy projekt (MQTT)
    start_mqtt_handler();

    // UWAGA KOLEŻNKI: Nigdy nie uruchamiać obu naraz, chyba że wiesz co robisz
    // (mogą się gryźć o WiFi albo zasoby).
}