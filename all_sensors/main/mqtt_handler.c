#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdlib.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "driver/i2c.h"
#include "driver/gpio.h"

#include "mqtt_client.h"
#include "cJSON.h"
#include "ssd1306.h"      
#include "mqtt_handler.h"

#include "driver/gpio.h"
#include "driver/i2c.h"

// Odwołanie do struktury OLED zdefiniowanej w main.c
extern SSD1306_t dev; 

// ================= KONFIGURACJA =================
#define ESP_WIFI_SSID       "marco"
#define ESP_WIFI_PASS       "polo1234"
#define ESP_MAXIMUM_RETRY   10

#define ESP_MQTT_BROKER_URL "mqtt://srv38.mikr.us:40133"
#define ESP_MQTT_USER       "guest"
#define ESP_MQTT_PASS       "guest"

#define TOPIC_ROOT          "smartmirror/user_01"

#define I2C_PORT            I2C_NUM_0
#define BH1750_ADDR         0x23
#define PIR_PIN             27

static const char *TAG = "SMART_MIRROR";
static char esp_mac_str[13];

static esp_mqtt_client_handle_t client = NULL;
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MQTT_CONNECTED_BIT BIT2
#define MQTT_CONNECTED_BIT BIT2

// Zmienne globalne stanu
char current_display_text[64] = "Czesc Piekna, czekam na dane...";
bool is_screen_on = true;

// --- FUNKCJA ODŚWIEŻANIA EKRANU ---
void refresh_oled() {
    if (is_screen_on) {
        // Linia 2 to środek ekranu w Twojej konfiguracji
        ssd1306_display_text(&dev, 2, current_display_text, strlen(current_display_text), false);
    } else {
        ssd1306_clear_screen(&dev, false);
        ESP_LOGI(TAG, "[EDGE] Ekran wygaszony");
    }
}

// --- OBSŁUGA KOMEND PRZYCHODZĄCYCH (JSON) ---
void handle_command_json(const char *json_str) {
    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) return;

    cJSON *action = cJSON_GetObjectItem(root, "action");
    if (cJSON_IsString(action)) {
        if (strcmp(action->valuestring, "update_text") == 0) {
            cJSON *msg = cJSON_GetObjectItem(root, "msg");
            if (cJSON_IsString(msg)) {
                snprintf(current_display_text, sizeof(current_display_text), "%s", msg->valuestring);
                refresh_oled();
            }
        }
        else if (strcmp(action->valuestring, "set_screen") == 0) {
            cJSON *state = cJSON_GetObjectItem(root, "state");
            cJSON *state = cJSON_GetObjectItem(root, "state");
            if (cJSON_IsString(state)) {
                is_screen_on = (strcmp(state->valuestring, "ON") == 0);
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

// --- HANDLER EVENTÓW MQTT ---
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
            char *data_buf = malloc(event->data_len + 1);
            if (data_buf) {
                memcpy(data_buf, event->data, event->data_len);
                data_buf[event->data_len] = '\0';
                handle_command_json(data_buf);
                free(data_buf);
            }
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT Rozłączono!");
        xEventGroupClearBits(s_wifi_event_group, MQTT_CONNECTED_BIT);
        break;
    default: break;
    }
}

// --- ZADANIE 1: TELEMETRIA ---
void telemetry_task(void *pvParameters) {
    char topic[128];
    char payload[256];
    snprintf(topic, sizeof(topic), "%s/%s/telemetry", TOPIC_ROOT, esp_mac_str);
    uint8_t sensor_data[2];

    while (1) {
        if (xEventGroupGetBits(s_wifi_event_group) & MQTT_CONNECTED_BIT) {
            float temp = 21.0 + ((esp_random() % 50) / 10.0);
            float lux = 0;
            float hum = 45.0 + ((esp_random() % 200) / 10.0);
            esp_err_t err = i2c_master_read_from_device(I2C_PORT, BH1750_ADDR, sensor_data, 2, 100 / portTICK_PERIOD_MS);

            if (err == ESP_OK) {
                lux = ((sensor_data[0] << 8) | sensor_data[1]) / 1.2;
            }

            // Zmieniamy "lux" na "lux_raw", żeby backend był szczęśliwy
            snprintf(payload, sizeof(payload),
                "{\"temp\": %.2f, \"hum\": %.1f, \"lux_raw\": %.1f, \"unit_t\": \"C\", \"unit_h\": \"%%\", \"unit_l\": \"lx\"}",
                temp, hum, lux);

            esp_mqtt_client_publish(client, topic, payload, 0, 0, 0);
            ESP_LOGI(TAG, "Wysłano dane: %s", payload);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// --- ZADANIE 2: STATUS TECHNICZNY ---
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

// --- ZADANIE 3: WYKRYWANIE RUCHU ---
void motion_task(void *pvParameters) {
    char topic[128];
    snprintf(topic, sizeof(topic), "%s/%s/event", TOPIC_ROOT, esp_mac_str);
    bool last_motion = false;
    while(1) {
        bool motion_detected = gpio_get_level(PIR_PIN);
        if (motion_detected != last_motion) {
            last_motion = motion_detected;
            is_screen_on = motion_detected;
            refresh_oled();
            if (xEventGroupGetBits(s_wifi_event_group) & MQTT_CONNECTED_BIT) {
                char payload[64];
                snprintf(payload, sizeof(payload), "{\"type\": \"motion\", \"val\": %s}",
                    motion_detected ? "true" : "false");
                esp_mqtt_client_publish(client, topic, payload, 0, 0, 0);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// --- OBSŁUGA WIFI ---
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
        ESP_LOGI(TAG, "WiFi Połączone!");
    }
}

void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default()); // TO MUSI BYĆ TU
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = { .ssid = ESP_WIFI_SSID, .password = ESP_WIFI_PASS },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
}

static void mqtt_app_start(void) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = ESP_MQTT_BROKER_URL,
        .credentials = { .username = ESP_MQTT_USER, .authentication = { .password = ESP_MQTT_PASS } },
    };
    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}

// --- GŁÓWNA FUNKCJA STARTOWA (POPRAWIONA KOLEJNOŚĆ) ---
void start_mqtt_handler(void) {
    // 1. Inicjalizacja NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Pobranie MAC
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(esp_mac_str, sizeof(esp_mac_str), "%02X%02X%02X%02X%02X%02X", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);

    // 3. Informacja na OLED (dev musi być zainicjalizowane w main.c wcześniej!)
    ssd1306_display_text(&dev, 0, "Booting System...", 17, false);

    // 4. Inicjalizacja Sieci
    wifi_init_sta(); 

    // 5. Start MQTT
    mqtt_app_start();

    // 6. Start zadań FreeRTOS
    xTaskCreate(telemetry_task, "telemetry", 4096, NULL, 5, NULL);
    xTaskCreate(status_task, "status", 4096, NULL, 5, NULL);
    xTaskCreate(motion_task, "motion", 4096, NULL, 5, NULL);
}