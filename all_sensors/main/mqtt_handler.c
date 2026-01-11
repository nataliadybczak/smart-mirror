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
#include "driver/ledc.h"  // Potrzebne do sterowania jasnością LED
#include "mqtt_handler.h"

// --- KONFIGURACJA ---
#define ESP_WIFI_SSID       "marco"
#define ESP_WIFI_PASS       "polo1234"
#define ESP_MQTT_BROKER_URL "mqtt://srv38.mikr.us:40133"
#define TOPIC_ROOT          "smartmirror/user_01"

#define PIR_PIN             27
#define LED_PIN             2    // Pin diody LED
#define T_SCREEN_CH         TOUCH_PAD_NUM4 // GPIO 13
#define T_POWER_CH          TOUCH_PAD_NUM6 // GPIO 14
#define T_PARTY_CH          TOUCH_PAD_NUM5 // GPIO 12
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
char current_display_text[64] = "Lusterko aktywne"; // Tekst z serwera

static int mirror_timer = 600; 
static int lockout_timer = 0; 
static bool is_mirror_on = true; 
static bool is_party_mode = false; 
static int default_on_time = 600; 

// --- PROTOTYPY ---
void refresh_oled(void);
void set_led_brightness(int percent);
static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data);
static void mqtt_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data);
void wifi_init_sta(void);
static void mqtt_app_start(void);
void telemetry_task(void *pvParameters);
void button_task(void *pvParameters);
void system_timer_task(void *pvParameters);
void handle_command_json(const char *json_str);

extern SSD1306_t dev;
extern void send_dfplayer_cmd(uint8_t cmd, uint16_t dat);

// --- STEROWANIE LED ---
void set_led_brightness(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    uint32_t duty = (percent * 8191) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

// --- FUNKCJA WYSWIETLANIA ---
void refresh_oled(void) {
    ssd1306_clear_screen(&dev, false);
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
                ssd1306_display_text(&dev, 6, current_display_text, strlen(current_display_text), false);
            }
            break;

        case 1: // POGODA
            ssd1306_display_text(&dev, 0, "--- POGODA ---", 14, false);
            snprintf(buf, sizeof(buf), "Temp: %.1f C", g_temp);
            ssd1306_display_text(&dev, 2, buf, strlen(buf), false);
            snprintf(buf, sizeof(buf), "Wilg: %.1f %%", g_hum);
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

// --- LOGIKA CZASU ---
void system_timer_task(void *pvParameters) {
    while(1) {
        if (mirror_timer > 0) {
            mirror_timer--;
            if (mirror_timer == 0) {
                is_mirror_on = false;
                send_dfplayer_cmd(0x03, 3); // Muzyczka OFF
                refresh_oled();
            }
        }
        if (lockout_timer > 0) lockout_timer--;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// --- DOTYK ---
void button_task(void *pvParameters) {
    touch_pad_init();
    touch_pad_config(T_SCREEN_CH, 0); touch_pad_config(T_POWER_CH, 0); touch_pad_config(T_PARTY_CH, 0);
    uint16_t v1, v2, v3;
    int pwr_hold = 0;

    while(1) {
        touch_pad_read(T_SCREEN_CH, &v1); touch_pad_read(T_POWER_CH, &v2); touch_pad_read(T_PARTY_CH, &v3);

        if (v1 < TOUCH_THRESH) {
            current_screen = (current_screen + 1) % 4;
            refresh_oled(); vTaskDelay(pdMS_TO_TICKS(300));
        }

        if (v2 < TOUCH_THRESH) {
            pwr_hold++;
            if (pwr_hold == 20) {
                if (is_mirror_on) {
                    is_mirror_on = false; mirror_timer = 0; lockout_timer = 600;
                    send_dfplayer_cmd(0x03, 3);
                } else {
                    is_mirror_on = true; mirror_timer = default_on_time; lockout_timer = 0;
                    send_dfplayer_cmd(0x03, 2);
                }
                refresh_oled();
            }
        } else { pwr_hold = 0; }

        if (v3 < TOUCH_THRESH) {
            is_party_mode = !is_party_mode;
            if (is_party_mode) {
                uint16_t songs[] = {4, 5, 6};
                int idx = esp_random() % 3;
                send_dfplayer_cmd(0x01, songs[idx]);
            } else {
                send_dfplayer_cmd(0x0E, 0);
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        if (current_screen == 3 && is_mirror_on) {
            static int bc = 0; if (++bc > 20) { cat_blink = !cat_blink; refresh_oled(); bc = 0; }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// --- SENSORY ---
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

        if (gpio_get_level(PIR_PIN) && lockout_timer == 0) {
            if (!is_mirror_on) { is_mirror_on = true; send_dfplayer_cmd(0x03, 2); }
            mirror_timer = default_on_time;
        }

        if (g_lux > 600.0 && music_11s_timer == 0) {
            send_dfplayer_cmd(0x12, 1); music_11s_timer = 11;
        }
        if (music_11s_timer > 0) {
            music_11s_timer--;
            if (music_11s_timer == 0) send_dfplayer_cmd(0x0E, 0);
        }

        if (xEventGroupGetBits(s_wifi_event_group) & MQTT_CONNECTED_BIT) {
            char p[256];
            // --- POPRAWKA: Musimy wysłać "press", bo inaczej Python Natalii wywali błąd ---
            snprintf(p, sizeof(p), 
                    "{\"temp\": %.1f, \"hum\": %.1f, \"lux\": %.1f, \"press\": %.1f, \"time\": %d}", 
                    g_temp, g_hum, g_lux, g_press, mirror_timer);
                    
            char pub_topic[128];
            snprintf(pub_topic, sizeof(pub_topic), "%s/%s/telemetry", TOPIC_ROOT, esp_mac_str);
            
            esp_mqtt_client_publish(client, pub_topic, p, 0, 0, 0);
        }
        
        refresh_oled();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// --- MQTT CMD (TWOJE LOGI 🙀) ---
void handle_command_json(const char *json_str) {
    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) return;
    cJSON *action = cJSON_GetObjectItem(root, "action");
    if (cJSON_IsString(action)) {
        if (strcmp(action->valuestring, "update_text") == 0) {
            cJSON *msg = cJSON_GetObjectItem(root, "msg");
            if (cJSON_IsString(msg)) {
                snprintf(current_display_text, sizeof(current_display_text), "%s", msg->valuestring);
                ESP_LOGI(TAG, "🙀 NOWY TEKST NA EKRAN: %s", current_display_text);
                refresh_oled();
            }
        }
        else if (strcmp(action->valuestring, "set_screen") == 0) {
            cJSON *state = cJSON_GetObjectItem(root, "state");
            if (cJSON_IsString(state)) {
                is_mirror_on = (strcmp(state->valuestring, "ON") == 0);
                ESP_LOGI(TAG, "🙀 ZMIANA STANU EKRANU: %s", is_mirror_on ? "ON" : "OFF");
                refresh_oled();
            }
        }
        else if (strcmp(action->valuestring, "set_light") == 0) {
             cJSON *val = cJSON_GetObjectItem(root, "value");
             if (cJSON_IsNumber(val)) {
                 ESP_LOGI(TAG, "🙀 ZMIANA LED: %d%%", val->valueint);
                 set_led_brightness(val->valueint);
             }
        }
        else if (strcmp(action->valuestring, "set_volume") == 0) {
             cJSON *val = cJSON_GetObjectItem(root, "value");
             if (cJSON_IsNumber(val)) {
                 ESP_LOGI(TAG, "🙀 ZMIANA GŁOŚNOŚCI: %d%%", val->valueint);
                 send_dfplayer_cmd(0x06, (uint16_t)val->valueint);
             }
        }
    }
    cJSON_Delete(root);
}

// --- RESZTA BOILERPLATE ---
static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    if (id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (id == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(s_wifi_event_group, BIT0);
}

static void mqtt_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    esp_mqtt_event_handle_t e = data;
    if (id == MQTT_EVENT_CONNECTED) {
        xEventGroupSetBits(s_wifi_event_group, MQTT_CONNECTED_BIT);
        
        // --- POPRAWKA: Dodajemy MAC do tematu, żeby pasowało do kodu Natalii ---
        char sub_topic[128];
        snprintf(sub_topic, sizeof(sub_topic), "%s/%s/cmd", TOPIC_ROOT, esp_mac_str);
        esp_mqtt_client_subscribe(client, sub_topic, 0);
        
        ESP_LOGI(TAG, "MQTT Polaczono! Slucham na: %s", sub_topic);
    } else if (id == MQTT_EVENT_DATA) {
        ESP_LOGW(TAG, "ODEBRANO COŚ! Treść: %.*s", e->data_len, e->data);
        char *b = malloc(e->data_len + 1); 
        memcpy(b, e->data, e->data_len); 
        b[e->data_len] = 0;
        handle_command_json(b); 
        free(b);
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
    
    // PWM LED
    ledc_timer_config_t lt = { .speed_mode=0, .timer_num=0, .duty_resolution=13, .freq_hz=5000, .clk_cfg=0 };
    ledc_timer_config(&lt);
    ledc_channel_config_t lc = { .speed_mode=0, .channel=0, .timer_sel=0, .intr_type=0, .gpio_num=LED_PIN, .duty=0 };
    ledc_channel_config(&lc);

    gpio_set_direction(PIR_PIN, GPIO_MODE_INPUT);
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL); esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init(); setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1); tzset();

    wifi_init_sta(); mqtt_app_start();
    
    xTaskCreate(button_task, "button", 3072, NULL, 10, NULL);
    xTaskCreate(telemetry_task, "telemetry", 4096, NULL, 5, NULL);
    xTaskCreate(system_timer_task, "sys_timer", 2048, NULL, 5, NULL);
}