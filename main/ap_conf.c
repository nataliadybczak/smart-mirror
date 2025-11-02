#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "ap_conf.h"

#define WIFI_SSID_ap "ESP32_sama_frajda"
#define WIFI_PASS_ap "12345678"
#define MAX_STA_CONN 4

#define WIFI_SSID "kalarepa"
#define WIFI_PASS "123456789"

static const char *TAG = "wifi_ap";

extern void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

void wifi_init_softap(void)
{
    // Inicjalizacja sieci i event loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Konfiguracja Access Pointa
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_SSID_ap,
            .ssid_len = strlen(WIFI_SSID_ap),
            .channel = 1,
            .password = WIFI_PASS_ap,
            .max_connection = MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };

    if (strlen(WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Access Point uruchomiony");
    ESP_LOGI(TAG, "SSID: %s  |  hasło: %s", WIFI_SSID_ap, WIFI_PASS_ap);
}

void wifi_init_apsta(void)
{
    esp_netif_init();
    esp_event_loop_create_default();

    // Tworzymy interfejsy sieciowe
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 🔹 Rejestracja handlerów zdarzeń
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    // --- Konfiguracja AP ---
    wifi_config_t ap_config = {
        .ap = {
            .ssid = WIFI_SSID_ap,
            .ssid_len = 0,
            .channel = 1,
            .password = WIFI_PASS_ap,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };
    if (strlen(WIFI_PASS_ap) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    // --- Konfiguracja STA ---
    wifi_config_t sta_config = {0};
    strncpy((char*)sta_config.sta.ssid, WIFI_SSID, sizeof(sta_config.sta.ssid)-1);
    strncpy((char*)sta_config.sta.password, WIFI_PASS, sizeof(sta_config.sta.password)-1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));

    ESP_ERROR_CHECK(esp_wifi_start());

    //Po starcie – nawiąż połączenie w trybie STA
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI("wifi", "Tryb jednoczesny: STA + AP uruchomiony");
}

