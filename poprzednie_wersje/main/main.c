#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#define WIFI_SSID   "marco"
#define WIFI_PASS   "polo1234"
#define LED_GPIO    4

static const char *TAG = "wifi_test";

volatile bool wifi_connected = false; 
volatile bool last_wifi_connected = false;

static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0


// ====== HANDLER ZDARZEŃ =======
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {

        wifi_connected = false;
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);

        ESP_LOGI(TAG, "Rozłączono!");
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {

        wifi_connected = true;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);

        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Połączono! IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}


// ====== INICJALIZACJA AP + STA =======
void wifi_init_apsta(void)
{
    esp_netif_init();
    esp_event_loop_create_default();

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_event_group = xEventGroupCreate();

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    wifi_config_t sta_cfg = {0};
    strncpy((char*)sta_cfg.sta.ssid, WIFI_SSID, sizeof(sta_cfg.sta.ssid)-1);
    strncpy((char*)sta_cfg.sta.password, WIFI_PASS, sizeof(sta_cfg.sta.password)-1);

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = "to_patryk",
            .ssid_len = strlen("to_patryk"),
            .channel = 1,
            .password = "12345678",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK
        }
    };

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);

    esp_wifi_start();

    ESP_LOGI(TAG, "Tryb AP+STA uruchomiony");
}


// ====== MIGANIE DIODĄ =======
void blink_task(void *pv)
{
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);

    while (1) {
        EventBits_t bits = xEventGroupGetBits(wifi_event_group);

        if (bits & WIFI_CONNECTED_BIT) {
            gpio_set_level(LED_GPIO, 1);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        } else {
            gpio_set_level(LED_GPIO, 1);
            vTaskDelay(300 / portTICK_PERIOD_MS);
            gpio_set_level(LED_GPIO, 0);
            vTaskDelay(300 / portTICK_PERIOD_MS);
        }
    }
}


// ======= HTTP GET ZROBI0NY RĘCZNIE NA SOCKETACH ========
void http_get_task(void *pv)
{
    const char *host = "httpforever.com";

    while (1) {
        if (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT) {

            struct addrinfo hints = {
                .ai_family = AF_INET,
                .ai_socktype = SOCK_STREAM,
            };
            struct addrinfo *res;

            int err = getaddrinfo(host, "80", &hints, &res);
            if (err != 0 || res == NULL) {
                ESP_LOGE(TAG, "getaddrinfo failed");
                vTaskDelay(5000 / portTICK_PERIOD_MS);
                continue;
            }

            int sock = socket(res->ai_family, res->ai_socktype, 0);
            if (sock < 0) {
                ESP_LOGE(TAG, "socket error");
                freeaddrinfo(res);
                continue;
            }

            if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
                ESP_LOGE(TAG, "connect failed");
                close(sock);
                freeaddrinfo(res);
                continue;
            }

            freeaddrinfo(res);

            char request[] =
                "GET / HTTP/1.1\r\n"
                "Host: httpforever.com\r\n"
                "User-Agent: ESP32\r\n"
                "Connection: close\r\n\r\n";

            write(sock, request, strlen(request));

            char buffer[512];
            int r;
            while ((r = read(sock, buffer, sizeof(buffer)-1)) > 0) {
                buffer[r] = 0;
                printf("%s", buffer);
            }

            close(sock);
        }

        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
}


// ===== MAIN =====
void app_main(void)
{
    nvs_flash_init();
    wifi_init_apsta();

    xTaskCreate(blink_task, "blink_task", 2048, NULL, 5, NULL);
    xTaskCreate(http_get_task, "http_get_task", 8192, NULL, 5, NULL);
}
