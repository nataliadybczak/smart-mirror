#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_random.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "driver/i2c.h"
#include "driver/gpio.h"

#include "mqtt_client.h"
#include "cJSON.h"
#include "ssd1306.h"     // To musi być tutaj, żeby znać typ SSD1306_t
#include "mqtt_handler.h"

// --- KONFIGURACJA ---
#define ESP_WIFI_SSID       "kalarepa"
#define ESP_WIFI_PASS       "123456789"
#define ESP_MAXIMUM_RETRY   10

#define ESP_MQTT_BROKER_URL "mqtt://srv38.mikr.us:40133"
#define ESP_MQTT_USER       "guest"
#define ESP_MQTT_PASS       "guest"

#define TOPIC_ROOT          "smartmirror/user_01"

#define I2C_PORT I2C_NUM_0
#define BH1750_ADDR 0x23
#define PIR_PIN 27
// --------------------

static const char *TAG = "SMART_MIRROR";
static char esp_mac_str[13];

static esp_mqtt_client_handle_t client = NULL;
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MQTT_CONNECTED_BIT BIT2

// Pobieramy zmienną 'dev' (ekran) z pliku main.c
// Dzięki temu mqtt_handler.c widzi ekran zainicjalizowany w main.c
extern SSD1306_t dev;

char current_display_text[64] = "Czekam na dane...";
bool is_screen_on = true;

// Funkcja odświeżająca ekran
void refresh_oled() {
    if (is_screen_on) {
        // Podajemy adres &dev, bo funkcje wymagają wskaźnika
        ssd1306_clear_screen(&dev, false);
        ssd1306_display_text(&dev, 0, current_display_text, strlen(current_display_text), false);
    } else {
        ssd1306_clear_screen(&dev, false);
    }
}

void handle_command_json(const char *json_str) {
    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        ESP_LOGW(TAG, "Błędny JSON!");
        return;
    }

    cJSON *action = cJSON_GetObjectItem(root, "action");
    if (cJSON_IsString(action)) {
        if (strcmp(action->valuestring, "update_text") == 0) {
            cJSON *msg = cJSON_GetObjectItem(root, "msg");
            if (cJSON_IsString(msg)) {
                snprintf(current_display_text, sizeof(current_display_text), "%s", msg->valuestring);
                ESP_LOGI(TAG, "Nowy tekst: %s", current_display_text);
                refresh_oled();
            }
        }
        else if (strcmp(action->valuestring, "set_screen") == 0) {
            cJSON *state = cJSON_GetObjectItem(root, "state");
            if (cJSON_IsString(state)) {
                is_screen_on = (strcmp(state->valuestring, "ON") == 0);
                refresh_oled();
            }
        }
        else if (strcmp(action->valuestring, "set_light") == 0) {
             ESP_LOGI(TAG, "Zmieniono jasność (symulacja)");
        }
    }
    cJSON_Delete(root);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT Połączono!");
        xEventGroupSetBits(s_wifi_event_group, MQTT_CONNECTED_BIT);
        char sub_topic[128];
        snprintf(sub_topic, sizeof(sub_topic), "%s/%s/cmd", TOPIC_ROOT, esp_mac_str);
        esp_mqtt_client_subscribe(client, sub_topic, 0);
        break;
    case MQTT_EVENT_DATA:
        if (event->data_len > 0) {
            char *data_buf = (char *)malloc(event->data_len + 1);
            if (data_buf) {
                memcpy(data_buf, event->data, event->data_len);
                data_buf[event->data_len] = '\0';
                handle_command_json(data_buf);
                free(data_buf);
            }
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        xEventGroupClearBits(s_wifi_event_group, MQTT_CONNECTED_BIT);
        break;
    default: break;
    }
}

void telemetry_task(void *pvParameters) {
    char topic[128];
    char payload[256];
    snprintf(topic, sizeof(topic), "%s/%s/telemetry", TOPIC_ROOT, esp_mac_str);

    uint8_t sensor_data[2];

    while (1) {
        if (xEventGroupGetBits(s_wifi_event_group) & MQTT_CONNECTED_BIT) {
            // 1. Temp (Symulacja, bo brak czujnika)
            float temp = 21.0 + ((esp_random() % 50) / 10.0);

            // 2. Lux (Prawdziwy odczyt I2C)
            float lux = 0;
            // TU BYŁA LITERÓWKA - TERAZ JEST POPRAWNIE: esp_err_t err = ...
            esp_err_t err = i2c_master_read_from_device(I2C_PORT, BH1750_ADDR, sensor_data, 2, 100 / portTICK_PERIOD_MS);
            
            if (err == ESP_OK) {
                lux = ((sensor_data[0] << 8) | sensor_data[1]) / 1.2;
            } else {
                ESP_LOGW(TAG, "Błąd I2C: %s", esp_err_to_name(err));
            }

            snprintf(payload, sizeof(payload),
                "{\"temp\": %.2f, \"lux\": %.1f, \"unit_t\": \"C\", \"unit_l\": \"lx\"}",
                temp, lux);

            esp_mqtt_client_publish(client, topic, payload, 0, 0, 0);
            ESP_LOGI(TAG, "Telemetria: %s", payload);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void motion_task(void *pvParameters) {
    char topic[128];
    snprintf(topic, sizeof(topic), "%s/%s/event", TOPIC_ROOT, esp_mac_str);
    bool last_motion = false;

    while(1) {
        bool motion_detected = gpio_get_level(PIR_PIN);
        if (motion_detected != last_motion) {
            last_motion = motion_detected;
            if (motion_detected) {
                is_screen_on = true;
                snprintf(current_display_text, sizeof(current_display_text), "WIDZE CIE!");
            } else {
                is_screen_on = false;
            }
            refresh_oled();

            if (xEventGroupGetBits(s_wifi_event_group) & MQTT_CONNECTED_BIT) {
                char payload[64];
                snprintf(payload, sizeof(payload), "{\"type\": \"motion\", \"val\": %s}",
                    motion_detected ? "true" : "false");
                esp_mqtt_client_publish(client, topic, payload, 0, 0, 0);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void status_task(void *pvParameters) {
    char topic[128];
    char payload[256];
    snprintf(topic, sizeof(topic), "%s/%s/status", TOPIC_ROOT, esp_mac_str);

    while (1) {
        if (xEventGroupGetBits(s_wifi_event_group) & MQTT_CONNECTED_BIT) {
            int64_t uptime = esp_timer_get_time() / 1000000;
            esp_netif_ip_info_t ip_info;
            esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (netif) esp_netif_get_ip_info(netif, &ip_info);

            snprintf(payload, sizeof(payload),
                "{\"uptime\": %lld, \"ip\": \"" IPSTR "\", \"online\": true}",
                uptime, IP2STR(&ip_info.ip));
            esp_mqtt_client_publish(client, topic, payload, 0, 0, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        xEventGroupClearBits(s_wifi_event_group, MQTT_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = { .ssid = ESP_WIFI_SSID, .password = ESP_WIFI_PASS, .threshold.authmode = WIFI_AUTH_WPA2_PSK },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT) {
        esp_mqtt_client_start(client);
    }
}

static void mqtt_app_start(void) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = ESP_MQTT_BROKER_URL,
        .credentials = { .username = ESP_MQTT_USER, .authentication = { .password = ESP_MQTT_PASS } },
    };
    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
}

void start_mqtt_handler(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(esp_mac_str, sizeof(esp_mac_str), "%02X%02X%02X%02X%02X%02X", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    ESP_LOGI(TAG, "ID: %s", esp_mac_str);

    // TUTAJ JUZ NIE MA INICJALIZACJI EKRANU (robi to main.c)
    // Tylko startujemy sieć i zadania
    mqtt_app_start();
    wifi_init_sta();

    xTaskCreate(telemetry_task, "telemetry", 4096, NULL, 5, NULL);
    xTaskCreate(status_task, "status", 4096, NULL, 5, NULL);
    xTaskCreate(motion_task, "motion", 4096, NULL, 5, NULL);
}