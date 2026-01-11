#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "ssd1306.h"
#include "bme280.h"
#include "driver/gpio.h"
#include "driver/touch_pad.h"
#include "mqtt_handler.h"

// --- KONFIGURACJA (TUTAJ WPISZ SWOJE DANE) ---
#define ESP_WIFI_SSID       "marco"
#define ESP_WIFI_PASS       "polo1234"
#define ESP_MQTT_BROKER_URL "mqtt://srv38.mikr.us:40133"
#define TOPIC_ROOT          "smartmirror/user_01"

// --- PINY ---
#define PIR_PIN             27
#define T_SCREEN_CH         TOUCH_PAD_NUM4 // GPIO 13 (Kabelek 1)
#define T_POWER_CH          TOUCH_PAD_NUM6 // GPIO 14 (Kabelek 2)
#define T_PARTY_CH          TOUCH_PAD_NUM5 // GPIO 12 (Kabelek 3)
#define TOUCH_THRESH        450

static const char *TAG = "SMART_MIRROR";
static char esp_mac_str[13];
static esp_mqtt_client_handle_t client;
static EventGroupHandle_t s_wifi_event_group;
#define MQTT_CONNECTED_BIT BIT2

// --- ZMIENNE STANU ---
static int current_screen = 0;
static float g_temp, g_hum, g_press, g_lux;
static bool cat_blink = false;

static int mirror_timer = 600;      // 10 minut (600 sekund)
static int lockout_timer = 0;      // Odliczanie blokady PIR
static bool is_mirror_on = true;   // Czy ekran ma swiecic
static bool is_party_mode = false; 
static int default_on_time = 600;  // Czas ustawiany przez MQTT

// --- PROTOTYPY ---
void refresh_oled(void);
static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data);
static void mqtt_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data);
void wifi_init_sta(void);
static void mqtt_app_start(void);
void telemetry_task(void *pvParameters);
void button_task(void *pvParameters);
void system_timer_task(void *pvParameters);
void handle_command_json(const char *json_str);

// --- ZEWNETRZNE ---
extern SSD1306_t dev;
extern void send_dfplayer_cmd(uint8_t cmd, uint16_t dat);

// --- FUNKCJA WYSWIETLANIA ---
void refresh_oled(void) {
    ssd1306_clear_screen(&dev, false);
    
    // Jesli lusterko jest wylazone (mirror_timer == 0), nic nie wyswietlaj
    if (!is_mirror_on) return;

    char buf[32];
    switch(current_screen) {
        case 0: // CZAS + KOMUNIKAT
            time_t now; struct tm ti; time(&now); localtime_r(&now, &ti);
            strftime(buf, sizeof(buf), "%H:%M:%S", &ti);
            ssd1306_display_text(&dev, 1, "    GODZINA", 11, false);
            ssd1306_display_text(&dev, 3, buf, strlen(buf), true);
            
            if (gpio_get_level(PIR_PIN) && lockout_timer == 0) {
                ssd1306_display_text(&dev, 6, "  WIDZE CIE!  ", 14, false);
            } else {
                strftime(buf, sizeof(buf), "%d.%m.%Y", &ti);
                ssd1306_display_text(&dev, 6, buf, strlen(buf), false);
            }
            break;

        case 1: // POGODA
            ssd1306_display_text(&dev, 0, "--- POGODA ---", 14, false);
            snprintf(buf, sizeof(buf), "Temp: %.1f C", g_temp);
            ssd1306_display_text(&dev, 2, buf, strlen(buf), false);
            snprintf(buf, sizeof(buf), "Wilg: %.0f %%", g_hum);
            ssd1306_display_text(&dev, 4, buf, strlen(buf), false);
            snprintf(buf, sizeof(buf), "Cisn: %.0f hPa", g_press);
            ssd1306_display_text(&dev, 6, buf, strlen(buf), false);
            break;

        case 2: // SWIATLO
            ssd1306_display_text(&dev, 0, "--- SWIATLO ---", 15, false);
            snprintf(buf, sizeof(buf), "Lux: %.0f", g_lux);
            ssd1306_display_text(&dev, 2, buf, strlen(buf), false);
            if (g_lux > 600) ssd1306_display_text(&dev, 5, "ZA JASNO!", 9, true);
            else if (g_lux < 50) ssd1306_display_text(&dev, 5, "ZA CIEMNO...", 12, false);
            else ssd1306_display_text(&dev, 5, "SLAY QUEEN!", 11, false);
            break;

        case 3: // KOTEK
            if (cat_blink) ssd1306_display_text(&dev, 1, "   (=^~~^=)   ", 14, false);
            else           ssd1306_display_text(&dev, 1, "   (=^..^=)   ", 14, false);
            ssd1306_display_text(&dev, 4, "Jestes piekna!", 15, false);
            ssd1306_display_text(&dev, 6, "  MILEGO DNIA!", 14, false);
            break;
    }
}

// --- LOGIKA CZASU (1 SEKUNDA) ---
void system_timer_task(void *pvParameters) {
    while(1) {
        if (mirror_timer > 0) {
            mirror_timer--;
            if (mirror_timer == 0) {
                is_mirror_on = false;
                send_dfplayer_cmd(0x03, 2); // Muzyczka OFF
                refresh_oled();
            }
        }
        if (lockout_timer > 0) lockout_timer--;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// --- DOTYK (3 KABELKI) ---
void button_task(void *pvParameters) {
    touch_pad_init();
    touch_pad_config(T_SCREEN_CH, 0); touch_pad_config(T_POWER_CH, 0); touch_pad_config(T_PARTY_CH, 0);
    uint16_t v1, v2, v3;
    int pwr_hold = 0;

    while(1) {
        touch_pad_read(T_SCREEN_CH, &v1); touch_pad_read(T_POWER_CH, &v2); touch_pad_read(T_PARTY_CH, &v3);

        if (v1 < TOUCH_THRESH) { // Kabelek 1: Ekrany
            current_screen = (current_screen + 1) % 4;
            refresh_oled(); vTaskDelay(pdMS_TO_TICKS(300));
        }

        if (v2 < TOUCH_THRESH) { // Kabelek 2: Power (Przytrzymaj)
            pwr_hold++;
            if (pwr_hold == 20) { // ok 2 sekundy
                if (is_mirror_on) {
                    is_mirror_on = false; mirror_timer = 0; lockout_timer = 600;
                    send_dfplayer_cmd(0x03, 3); // Muzyczka OFF
                } else {
                    is_mirror_on = true; mirror_timer = default_on_time; lockout_timer = 0;
                    send_dfplayer_cmd(0x03, 2); // Muzyczka ON
                }
                refresh_oled();
            }
        } else { pwr_hold = 0; }

        if (v3 < TOUCH_THRESH) { // Kabelek 3: Party
            is_party_mode = !is_party_mode;

            if (is_party_mode) {
                // Tworzymy listę dozwolonych piosenek
                uint16_t allowed_songs[] = {4, 5, 6};
                
                // Losujemy indeks: 0, 1 lub 2
                int index = esp_random() % 3;
                uint16_t chosen_song = allowed_songs[index];

                ESP_LOGI(TAG, "!!! PARTY TIME !!! Losowy indeks: %d, Wybrana piosenka: %04u", index, chosen_song);
                
                // Wysyłamy komendę (Folder 1, Utwór chosen_song)
                send_dfplayer_cmd(0x01, chosen_song); 
            } else {
                ESP_LOGI(TAG, "STOP PARTY");
                send_dfplayer_cmd(0x0E, 0); // Pauza/Stop
            }
            vTaskDelay(pdMS_TO_TICKS(500)); // Debouncing (zabezpieczenie przed podwójnym kliknięciem)
        }

        if (current_screen == 3 && is_mirror_on) { // Blink Kotka
            static int bc = 0; if (++bc > 20) { cat_blink = !cat_blink; refresh_oled(); bc = 0; }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// --- SENSORY I RUCH ---
void telemetry_task(void *pvParameters) {
    bme280_init(I2C_NUM_0, 0x76);
    static int music_11s_timer = 0;

    while (1) {
        bme280_read_float_data(I2C_NUM_0, 0x76, &g_temp, &g_press, &g_hum);
        uint8_t cmd = 0x10, d[2];
        i2c_master_write_to_device(I2C_NUM_0, 0x23, &cmd, 1, 100);
        vTaskDelay(pdMS_TO_TICKS(180));
        if (i2c_master_read_from_device(I2C_NUM_0, 0x23, d, 2, 100) == ESP_OK) {
            g_lux = ((d[0] << 8) | d[1]) / 1.2;
        }

        // Ruch (PIR)
        if (gpio_get_level(PIR_PIN) && lockout_timer == 0) {
            if (!is_mirror_on) { is_mirror_on = true; send_dfplayer_cmd(0x03, 2); }
            mirror_timer = default_on_time; // Odswiez 10 minut
        }

        // Muzyka 11s (Swiatlo)
        if (g_lux > 600.0 && music_11s_timer == 0) {
            send_dfplayer_cmd(0x12, 1); music_11s_timer = 11;
        }
        if (music_11s_timer > 0) {
            music_11s_timer--;
            if (music_11s_timer == 0) send_dfplayer_cmd(0x0E, 0);
        }

        // MQTT Publish
        if (xEventGroupGetBits(s_wifi_event_group) & MQTT_CONNECTED_BIT) {
            char p[256];
            snprintf(p, sizeof(p), "{\"temp\": %.1f, \"hum\": %.1f, \"lux\": %.1f, \"time\": %d}", 
                     g_temp, g_hum, g_lux, mirror_timer);
            esp_mqtt_client_publish(client, TOPIC_ROOT "/telemetry", p, 0, 0, 0);
        }
        
        refresh_oled();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// --- MQTT CMD ---
void handle_command_json(const char *json_str) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root) return;
    cJSON *action = cJSON_GetObjectItem(root, "action");
    if (cJSON_IsString(action)) {
        if (strcmp(action->valuestring, "set_timer") == 0) {
            cJSON *v = cJSON_GetObjectItem(root, "value");
            if (cJSON_IsNumber(v)) default_on_time = v->valueint;
        }
    }
    cJSON_Delete(root);
}

// --- BOILERPLATE ---
static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    if (id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (id == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(s_wifi_event_group, BIT0);
}

static void mqtt_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    esp_mqtt_event_handle_t e = data;
    if (id == MQTT_EVENT_CONNECTED) {
        xEventGroupSetBits(s_wifi_event_group, MQTT_CONNECTED_BIT);
        esp_mqtt_client_subscribe(client, TOPIC_ROOT "/cmd", 0);
        ESP_LOGI(TAG, "MQTT Polaczono!");
    } else if (id == MQTT_EVENT_DATA) {
        char *b = malloc(e->data_len + 1); memcpy(b, e->data, e->data_len); b[e->data_len] = 0;
        handle_command_json(b); free(b);
    }
}

void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
    wifi_config_t wifi_cfg = { .sta = { .ssid = ESP_WIFI_SSID, .password = ESP_WIFI_PASS } };
    esp_wifi_set_mode(WIFI_MODE_STA); esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_start();
    xEventGroupWaitBits(s_wifi_event_group, BIT0, pdFALSE, pdFALSE, portMAX_DELAY);
}

static void mqtt_app_start(void) {
    esp_mqtt_client_config_t mc = { .broker.address.uri = ESP_MQTT_BROKER_URL };
    client = esp_mqtt_client_init(&mc);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}

void start_mqtt_handler(void) {
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(esp_mac_str, sizeof(esp_mac_str), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    gpio_set_direction(PIR_PIN, GPIO_MODE_INPUT);
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL); esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init(); setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1); tzset();

    wifi_init_sta(); mqtt_app_start();
    
    xTaskCreate(button_task, "button", 3072, NULL, 10, NULL);
    xTaskCreate(telemetry_task, "telemetry", 4096, NULL, 5, NULL);
    xTaskCreate(system_timer_task, "sys_timer", 2048, NULL, 5, NULL);
}