#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h> // Do losowania
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_random.h" // Generator losowy
#include "esp_mac.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "mqtt_client.h"
#include "cJSON.h"

// ================= KONFIGURACJA =================

// 1. WIFI
#define ESP_WIFI_SSID       "kalarepa"
#define ESP_WIFI_PASS       "123456789"
#define ESP_MAXIMUM_RETRY   10

// 2. BROKER (Mój VPS na mikrusie)
#define ESP_MQTT_BROKER_URL "mqtt://srv38.mikr.us:40133"
#define ESP_MQTT_USER       "guest"
#define ESP_MQTT_PASS       "guest"

#define TOPIC_ROOT          "smartmirror/user_01" 

// =================================================

static const char *TAG = "SMART_MIRROR";
static char esp_mac_str[13];

static esp_mqtt_client_handle_t client = NULL;
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
//#define MQTT_CONNECTED_BIT BIT2
char current_display_text[64] = "Cześć Piękna, ja czekam na dane...";
bool is_screen_on = true;



// Funkcja odświeżająca (Wirtualny) Ekran na podstawie stanu
void refresh_oled() {
    if (is_screen_on) {
        ssd1306_display_text(current_text_msg);
    } else {
        ssd1306_clear_screen();
        ESP_LOGI(TAG, "[EDGE] Ekran wygaszony (oszczędzanie energii)");
    }
}

// ============================================================
// OBSŁUGA PRZYHODZĄCYCH INFO  NA OLEDA
// ============================================================
void handle_incoming_data(const char *topic, int topic_len, const char *data, int data_len){
    char topic_str[128];
    char data_str[128];

    if (topic_len >= sizeof(topic_str)) topic_len = sizeof(topic_str) - 1;  // -1 bo one sie na 0 kończa
    if (data_len >= sizeof(data_str)) data_len = sizeof(data_str) - 1;

    strncpy(topic_str, topic, topic_len);
    topic_str[data_len] = '\0';

    strncpy(data_str, data, data_len);
    data_str[data_len] = '\0';

    ESP_LOGI(TAG, "Odebrano dane MQTT. Temat: %s, tag: %s", topic_str, data_str);

    //sprawdzamy czy to tekst motywacyjny
    if (strstr(topic_str, "/display/text") != NULL) {
        //tUTAJ W PRZYSZŁOŚCI BEDZIE: oled_display_text(data_str);
        ESP_LOGI(TAG, "Wyświetl na OLED: %s", data_str);
    }
    //sprawdzamy czy to klaendarz
    else if (strstr(topic_str, "/display/calendar") != NULL) {
        //TUTAJ W PRZYSZŁOŚCI BEDZIE: oled_display_calendar(data_str);
        ESP_LOGI(TAG, "Wyświetl kalendarz na OLED: %s", data_str);
    }

    //sprawdzamy czy to polecenie włączenia/wyłączenia ekranu - nwm czy bedzei czy tylko PIR zdecyduje
    else if (strstr(topic_str, "/cmd/screen") != NULL) {
        if (strcmp(data_str, "ON") == 0) {
            ESP_LOGI(TAG, ">>> [SYSTEM] Włączam ekran OLED");
        } else if (strcmp(data_str, "OFF") == 0) {
            ESP_LOGI(TAG, ">>> [SYSTEM] Wygaszam ekran OLED (oszczędzanie energii)");
        }
    }
}

//=============================================================
// OBSŁUGA MQTT
// ============================================================
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    (void)event;

    switch ((esp_mqtt_event_id_t)event_id) {
    
        case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        xEventGroupSetBits(s_wifi_event_group, MQTT_CONNECTED_BIT);

        // Subskrypcje tematów przychodzących (do OLED)
        {
            char topic[128];

            // 1. Teksty motywacyjne
            snprintf(topic, sizeof(topic), "%s/%s/display/text", TOPIC_ROOT, esp_mac_str);
            esp_mqtt_client_subscribe(client, topic, 0);

            // 2. Kalendarz
            snprintf(topic, sizeof(topic), "%s/%s/display/calendar", TOPIC_ROOT, esp_mac_str);
            esp_mqtt_client_subscribe(client, topic, 0);

            // 3. Polecenia ekranu
            snprintf(topic, sizeof(topic), "%s/%s/cmd/screen", TOPIC_ROOT, esp_mac_str);
            esp_mqtt_client_subscribe(client, topic, 0);
        }

        break;
    
        case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        xEventGroupClearBits(s_wifi_event_group, MQTT_CONNECTED_BIT);
        break;

        case MQTT_EVENT_DATA:
        // Tutaj wpadają dane przychodzące z serwera
        handle_incoming_data(event->topic, event->topic_len, event->data, event->data_len);
        break;
    
        case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
        break;
        
        default:
        break;
    }
}

// --- Obsługa WiFi ---
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Ponawiam WiFi...");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        xEventGroupClearBits(s_wifi_event_group, MQTT_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// --- Inicjalizacja WiFi ---
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
        .sta = {
            .ssid = ESP_WIFI_SSID,
            .password = ESP_WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT) {
        esp_mqtt_client_start(client);
    }
}

// --- Start MQTT ---
static void mqtt_app_start(void) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = ESP_MQTT_BROKER_URL,
        .credentials = {
            .username = ESP_MQTT_USER,
            .authentication = { .password = ESP_MQTT_PASS }
        },
    };
    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
}

// ============================================================
// ZADANIE SYMULUJĄCE CZUJNIKI SMART MIRROR
// ============================================================
void sensors_task(void *pvParameters) {
    char topic[128];
    char payload[128];

    // Symulowane zmienne
    float temp = 22.0;
    float hum = 45.0;
    int lux = 300;
    int motion = 0;

    while (1) {
        // Czekamy na połączenie MQTT
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, MQTT_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(2000));
        
        if (bits & MQTT_CONNECTED_BIT) {
            
            // 1. Symulacja zmian wartości (żeby wykresy żyły)
            temp += ((float)(esp_random() % 20) / 10.0) - 1.0; // wahania +/- 1 st
            if(temp < 18) temp = 18; 
            if(temp > 30) temp = 30;

            hum += ((float)(esp_random() % 40) / 10.0) - 2.0; // wahania +/- 2%
            
            lux = 200 + (esp_random() % 600); // 200 - 800 lux
            
            motion = (esp_random() % 10) > 7 ? 1 : 0; // 30% szans na wykrycie ruchu

            // 2. Wysyłanie TEMPERATURY
            snprintf(topic, sizeof(topic), "%s/%s/sensor/temp", TOPIC_ROOT, esp_mac_str);
            snprintf(payload, sizeof(payload), "{\"val\": %.2f, \"unit\": \"C\"}", temp);
            esp_mqtt_client_publish(client, topic, payload, 0, 0, 0);
            ESP_LOGI(TAG, "Wyslano TEMP: %s -> %s", topic, payload);

            // 3. Wysyłanie WILGOTNOŚCI
            snprintf(topic, sizeof(topic), "%s/%s/sensor/humidity", TOPIC_ROOT, esp_mac_str);
            snprintf(payload, sizeof(payload), "{\"val\": %.1f, \"unit\": \"%%\"}", hum);
            esp_mqtt_client_publish(client, topic, payload, 0, 0, 0);

            // 4. Wysyłanie ŚWIATŁA
            snprintf(topic, sizeof(topic), "%s/%s/sensor/light", TOPIC_ROOT, esp_mac_str);
            snprintf(payload, sizeof(payload), "{\"val\": %d, \"unit\": \"lx\"}", lux);
            esp_mqtt_client_publish(client, topic, payload, 0, 0, 0);

            // 5. Wysyłanie CZUJNIKA RUCHU (PIR)
            snprintf(topic, sizeof(topic), "%s/%s/sensor/motion", TOPIC_ROOT, esp_mac_str);
            snprintf(payload, sizeof(payload), "{\"val\": %d}", motion);
            esp_mqtt_client_publish(client, topic, payload, 0, 0, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(5000)); // Wyślij co 5 sekund
    }
}

void start_mqtt_handler(void) {
//    ESP_ERROR_CHECK(nvs_flash_init());
    
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(esp_mac_str, sizeof(esp_mac_str), "%02X%02X%02X", mac[3], mac[4], mac[5]); // Skrócony MAC (ostatnie 3 bajty)
    
    mqtt_app_start();
    wifi_init_sta();

    xTaskCreate(sensors_task, "sensors_task", 4096, NULL, 5, NULL);
}