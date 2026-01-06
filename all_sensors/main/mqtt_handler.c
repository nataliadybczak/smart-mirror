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
#include "driver/ledc.h"

#include "mqtt_client.h"
#include "cJSON.h"
#include "ssd1306.h"
#include "mqtt_handler.h"

// Biblioteka BME280 Twojej koleżanki
#include "bme280.h" 

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
#define BME280_ADDR 0x76    // Jeśli nie zadziała, spróbuj 0x77
#define PIR_PIN 27
#define LED_PIN 2           

// Konfiguracja PWM
#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_OUTPUT_IO          (LED_PIN)
#define LEDC_CHANNEL            LEDC_CHANNEL_0
#define LEDC_DUTY_RES           LEDC_TIMER_13_BIT 
#define LEDC_FREQUENCY          5000              

static const char *TAG = "SMART_MIRROR";
static char esp_mac_str[13];

static esp_mqtt_client_handle_t client = NULL;
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MQTT_CONNECTED_BIT BIT2

extern SSD1306_t dev;

char current_display_text[64] = "Czekam na dane...";
bool is_screen_on = true;

// --- EKRAN ---
void refresh_oled() {
    if (is_screen_on) {
        ssd1306_clear_screen(&dev, false);
        ssd1306_display_text(&dev, 0, current_display_text, strlen(current_display_text), false);
    } else {
        ssd1306_clear_screen(&dev, false);
    }
}

// --- PWM (LED) ---
void set_led_brightness(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    uint32_t duty = (percent * 8191) / 100;
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
    ESP_LOGI(TAG, "STEROWANIE: LED %d%%", percent);
}

// --- MQTT CMD ---
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
            if (cJSON_IsString(state)) {
                is_screen_on = (strcmp(state->valuestring, "ON") == 0);
                refresh_oled();
            }
        }
        else if (strcmp(action->valuestring, "set_light") == 0) {
             cJSON *val = cJSON_GetObjectItem(root, "value");
             if (cJSON_IsNumber(val)) set_led_brightness(val->valueint);
        }
    }
    cJSON_Delete(root);
}

// --- TELEMETRIA ---
void telemetry_task(void *pvParameters) {
    char topic[128];
    char payload[256];
    snprintf(topic, sizeof(topic), "%s/%s/telemetry", TOPIC_ROOT, esp_mac_str);
    
    uint8_t sensor_data[2];

    // 1. Inicjalizacja BME280
    esp_err_t bme_status = bme280_init(I2C_PORT, BME280_ADDR);
    if (bme_status == ESP_OK) {
        ESP_LOGI(TAG, "BME280 wykryty!");
    } else {
        ESP_LOGE(TAG, "Nie znaleziono BME280 (Błąd: %d)", bme_status);
    }

    while (1) {
        if (xEventGroupGetBits(s_wifi_event_group) & MQTT_CONNECTED_BIT) {
            
            float temp = 0, press = 0, hum = 0, lux = 0;

            // 2. ODCZYT BME280
            if (bme280_read_float_data(I2C_PORT, BME280_ADDR, &temp, &press, &hum) != ESP_OK) {
                ESP_LOGW(TAG, "Błąd odczytu BME280 - wstawiam symulację");
                // Fallback, żeby coś wysłać
                temp = 22.0; 
                hum = 45.0;
                press = 1013.0;
            }

            // 3. ODCZYT BH1750 (Lux)
            uint8_t cmd = 0x01; 
            i2c_master_write_to_device(I2C_PORT, BH1750_ADDR, &cmd, 1, 100/portTICK_PERIOD_MS);
            cmd = 0x10;
            i2c_master_write_to_device(I2C_PORT, BH1750_ADDR, &cmd, 1, 100/portTICK_PERIOD_MS);
            vTaskDelay(pdMS_TO_TICKS(180));
            if (i2c_master_read_from_device(I2C_PORT, BH1750_ADDR, sensor_data, 2, 100/portTICK_PERIOD_MS) == ESP_OK) {
                lux = ((sensor_data[0] << 8) | sensor_data[1]) / 1.2;
            }

            // Budowa JSON (BEZ BATERII)
            snprintf(payload, sizeof(payload),
                "{\"temp\": %.2f, \"hum\": %.1f, \"press\": %.1f, \"lux\": %.1f}",
                temp, hum, press, lux);

            esp_mqtt_client_publish(client, topic, payload, 0, 0, 0);
            ESP_LOGI(TAG, "Dane: %s", payload);
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
            
            // Logika lokalna (Ekran/LED)
            if (motion_detected) {
                is_screen_on = true; set_led_brightness(100); refresh_oled();
            } else {
                is_screen_on = false; set_led_brightness(0); refresh_oled();
            }

            // Logika MQTT (Wysyłanie zdarzenia)
            if (xEventGroupGetBits(s_wifi_event_group) & MQTT_CONNECTED_BIT) {
                char payload[64];
                // Klucz "val" określa czy ruch jest (true) czy go nie ma (false)
                snprintf(payload, sizeof(payload), "{\"type\": \"motion\", \"val\": %s}", 
                    motion_detected ? "true" : "false");
                
                esp_mqtt_client_publish(client, topic, payload, 0, 0, 0);
                ESP_LOGI(TAG, "EVENT RUCHU: %s", payload);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200)); // Sprawdzaj często (responsywność)
    }
}

void status_task(void *pvParameters) {
    char topic[128]; char payload[256]; snprintf(topic, sizeof(topic), "%s/%s/status", TOPIC_ROOT, esp_mac_str);
    while (1) {
        if (xEventGroupGetBits(s_wifi_event_group) & MQTT_CONNECTED_BIT) {
            int64_t uptime = esp_timer_get_time() / 1000000;
            snprintf(payload, sizeof(payload), "{\"uptime\": %lld, \"online\": true}", uptime);
            esp_mqtt_client_publish(client, topic, payload, 0, 0, 0);
        } vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < ESP_MAXIMUM_RETRY) { esp_wifi_connect(); s_retry_num++; }
        xEventGroupClearBits(s_wifi_event_group, MQTT_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_retry_num = 0; xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init()); ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT(); ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_t instance_any_id, instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));
    wifi_config_t wifi_config = { .sta = { .ssid = ESP_WIFI_SSID, .password = ESP_WIFI_PASS } };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA)); ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    if (xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY) & WIFI_CONNECTED_BIT) {
        esp_mqtt_client_start(client);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        xEventGroupSetBits(s_wifi_event_group, MQTT_CONNECTED_BIT);
        char sub_topic[128]; snprintf(sub_topic, sizeof(sub_topic), "%s/%s/cmd", TOPIC_ROOT, esp_mac_str);
        esp_mqtt_client_subscribe(client, sub_topic, 0); break;
    case MQTT_EVENT_DATA:
        if (event->data_len > 0) {
            char *data_buf = (char *)malloc(event->data_len + 1);
            memcpy(data_buf, event->data, event->data_len); data_buf[event->data_len] = 0;
            handle_command_json(data_buf); free(data_buf);
        } break;
    case MQTT_EVENT_DISCONNECTED: xEventGroupClearBits(s_wifi_event_group, MQTT_CONNECTED_BIT); break;
    default: break;
    }
}

static void mqtt_app_start(void) {
    esp_mqtt_client_config_t mqtt_cfg = { .broker.address.uri = ESP_MQTT_BROKER_URL, .credentials = { .username = ESP_MQTT_USER, .authentication = { .password = ESP_MQTT_PASS } } };
    client = esp_mqtt_client_init(&mqtt_cfg); esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
}

void start_mqtt_handler(void) {
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(esp_mac_str, sizeof(esp_mac_str), "%02X%02X%02X%02X%02X%02X", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    ESP_LOGI(TAG, "ID: %s", esp_mac_str);
    
    // PWM Init
    ledc_timer_config_t ledc_timer = { .speed_mode=LEDC_MODE, .timer_num=LEDC_TIMER, .duty_resolution=LEDC_DUTY_RES, .freq_hz=LEDC_FREQUENCY, .clk_cfg=LEDC_AUTO_CLK };
    ledc_timer_config(&ledc_timer);
    ledc_channel_config_t ledc_channel = { .speed_mode=LEDC_MODE, .channel=LEDC_CHANNEL, .timer_sel=LEDC_TIMER, .intr_type=LEDC_INTR_DISABLE, .gpio_num=LEDC_OUTPUT_IO, .duty=0, .hpoint=0 };
    ledc_channel_config(&ledc_channel);

    // ADC usunięte całkowicie :)

    mqtt_app_start(); wifi_init_sta();
    xTaskCreate(telemetry_task, "telemetry", 4096, NULL, 5, NULL);
    xTaskCreate(status_task, "status", 4096, NULL, 5, NULL);
    xTaskCreate(motion_task, "motion", 4096, NULL, 5, NULL);
}