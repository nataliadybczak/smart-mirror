#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_http_client.h"
#include "driver/gpio.h"


#define WIFI_SSID      "marco"
#define WIFI_PASS      "polo1234"
#define LED_GPIO 4

static const char *TAG = "wifi_test";
static bool wifi_connected = false;
static bool last_wifi_connected = false;


    // --- Obsługa zdarzeń Wi-Fi ---
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
        if (last_wifi_connected != wifi_connected) {
            ESP_LOGI(TAG, "Rozłączono!");
            last_wifi_connected = wifi_connected;
        }
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        wifi_connected = true;
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        if (last_wifi_connected != wifi_connected) {
            ESP_LOGI(TAG, "Połączono! IP: " IPSTR, IP2STR(&event->ip_info.ip));
            last_wifi_connected = wifi_connected;
        }
    }
}

void wifi_init_sta(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid)-1);
    strncpy((char*)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password)-1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Inicjalizacja Wi-Fi zakończona");
}

// --- Symulacja mrugania diodą ---
static const char *TAG_LED = "led_task";  // osobny tag dla logów LED

void log_task(void *pvParameter)
{
    while (1) {
        if (!wifi_connected) {
            ESP_LOGW(TAG_LED, "mrug mrug");  // tylko logowanie, żadnego LED
        }
        vTaskDelay(500 / portTICK_PERIOD_MS); // co 500 ms
    }
}

void blink_task(void *pvParameter)
{
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);

    while (1) {
        if (!wifi_connected) {
            gpio_set_level(LED_GPIO, 1);
            vTaskDelay(500 / portTICK_PERIOD_MS);
            gpio_set_level(LED_GPIO, 0);
            vTaskDelay(500 / portTICK_PERIOD_MS);
        } else {
            gpio_set_level(LED_GPIO, 1);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
    }
}


// --- Test HTTP GET ---
void http_get_task(void *pvParameter)
{
    const char *url = "http://shinyoldlushtreasure.neverssl.com/online/";
    while (1) {
        if (wifi_connected) {
            esp_http_client_config_t config = {
                .url = url,
                .method = HTTP_METHOD_GET,
                .timeout_ms = 5000,
                .skip_cert_common_name_check = true,
            };
            esp_http_client_handle_t client = esp_http_client_init(&config);
            esp_err_t err = esp_http_client_perform(client);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "HTTP GET OK, status=%d, content_length=%d",
                         esp_http_client_get_status_code(client),
                         esp_http_client_get_content_length(client));
            } else {
                ESP_LOGE(TAG, "HTTP GET failed: %s", esp_err_to_name(err));
            }
            esp_http_client_cleanup(client);
        }
        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
}

// --- Funkcja główna ---
void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init_sta();

    // Task tylko dla diody
    xTaskCreate(&blink_task, "blink_task", 2048, NULL, 5, NULL);

    // Task tylko dla logów „mrug mrug”
    xTaskCreate(&log_task, "log_task", 2048, NULL, 5, NULL);

    // Task dla HTTP GET
    xTaskCreate(&http_get_task, "http_get_task", 8192, NULL, 5, NULL);
}

