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
#include "driver/ledc.h"
#include "mqtt_handler.h"

// --- KONFIGURACJA ---
#define ESP_MQTT_BROKER_URL "mqtt://srv38.mikr.us:40133"
#define TOPIC_ROOT "smartmirror/user_01"

// PINY I ADRESY
#define PIR_PIN 27
#define LED_PIN 2
#define I2C_PORT_NUM I2C_NUM_0
#define BME_ADDR 0x76
#define BH1750_ADDR 0x23
#define RTC_ADDR 0x68

// DOTYK
#define T_SCREEN_CH TOUCH_PAD_NUM4 // GPIO 13
#define T_POWER_CH TOUCH_PAD_NUM6  // GPIO 14
#define T_PARTY_CH TOUCH_PAD_NUM5  // GPIO 12
#define TOUCH_THRESH 450

static const char *TAG = "SMART_MIRROR";
static char esp_mac_str[13];
static esp_mqtt_client_handle_t client;
static EventGroupHandle_t s_wifi_event_group;
#define MQTT_CONNECTED_BIT BIT2
#define WIFI_CONNECTED_BIT BIT0

// --- ZMIENNE STANU ---
static int current_screen = 0;
static float g_temp = 0, g_hum = 0, g_press = 0, g_lux = 0;
static bool cat_blink = false;
char current_display_text[64] = "Lusterko aktywne";

static int mirror_timer = 10;           // pozostały czas świecenia lusterka (sekundy)
static int default_mirror_timeout = 30; // domyślny czas do wygaszenia lusterka
static int lockout_timer = 0;           // licznik blokady PIR (sekundy)
static int pir_lockout_duration = 5;    // czas blokady PIR po ręcznym wyłączeniu
static bool is_mirror_on = true;
static bool is_party_mode = false;
static bool lux_music_played = false;
static int music_11s_timer = 0;

// Zmienne zewnętrzne z main.c
extern SSD1306_t dev;
extern void send_dfplayer_cmd(uint8_t cmd, uint16_t dat);
extern void wifi_ble_force_erase_credentials(void);

// --- PROTOTYPY FUNKCJI (Aby uniknąć błędów implicit declaration) ---
void mqtt_app_start(void);
void refresh_oled(void);

// Konwersja formatów dla zegarka
uint8_t dec_to_bcd(int val) { return (uint8_t)((val / 10 << 4) | (val % 10)); }
int bcd_to_dec(uint8_t val) { return (int)(((val >> 4) * 10) + (val & 0x0F)); }

// Ustawianie czasu w module RTC
void rtc_set_time(int h, int m, int s, int d, int mo, int y)
{
    uint8_t data[8] = {
        0x00, // Rejestr startowy
        dec_to_bcd(s), dec_to_bcd(m), dec_to_bcd(h),
        0x01, // Dzień tygodnia (pominiecie)
        dec_to_bcd(d), dec_to_bcd(mo), dec_to_bcd(y - 2000)};
    i2c_master_write_to_device(I2C_NUM_0, RTC_ADDR, data, 8, 100);
}

// --- EVENT HANDLERY ---

// static void provisioning_event_handler(void* arg, esp_event_base_t event_base,
//                                       int32_t event_id, void* event_data) {
//     if (event_id == WIFI_PROV_START) {
//         ESP_LOGI(TAG, "Rozpoczęto tryb parowania...");
//         is_provisioning = true;
//     } else if (event_id == WIFI_PROV_END) {
//         ESP_LOGI(TAG, "Koniec parowania.");
//         is_provisioning = false;
//         wifi_prov_mgr_deinit();
//     }
// }

// Synchronizacja czasu systemowego ESP32 z modułu RTC
void sync_system_time_from_rtc()
{
    uint8_t reg = 0x00;
    uint8_t data[7];
    if (i2c_master_write_read_device(I2C_NUM_0, RTC_ADDR, &reg, 1, data, 7, 100) == ESP_OK)
    {
        struct tm tm;
        tm.tm_sec = bcd_to_dec(data[0]);
        tm.tm_min = bcd_to_dec(data[1]);
        tm.tm_hour = bcd_to_dec(data[2]);
        tm.tm_mday = bcd_to_dec(data[4]);
        tm.tm_mon = bcd_to_dec(data[5]) - 1;
        tm.tm_year = bcd_to_dec(data[6]) + 100;

        struct timeval tv = {.tv_sec = mktime(&tm)};
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "Zsynchronizowano czas z RTC: %02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
}

// --- STEROWANIE LED (Sprzętowe) ---
void set_led_brightness(int percent)
{
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;

    // Konwersja procent na duty cycle (13 bit -> max 8191)
    uint32_t duty = (percent * 8191) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ESP_LOGI(TAG, "LED ustawiono na %d%% (Duty: %lu)", percent, duty);
}

// --- EKRAN OLED (Sprzętowy) ---
void refresh_oled(void)
{
    static int last_screen = -1;
    static bool last_is_mirror_on = true;

    if (is_mirror_on != last_is_mirror_on)
    {
        ssd1306_clear_screen(&dev, false);
        last_is_mirror_on = is_mirror_on;
    }
    if (!is_mirror_on)
        return;

    // if (is_provisioning) {
    //     ssd1306_display_text(&dev, 0, " TRYB PAROWANIA", 15, true);
    //     ssd1306_display_text(&dev, 2, "Siec: PROV_LUST", 15, false);
    //     char pin_buf[20];
    //     snprintf(pin_buf, sizeof(pin_buf), "PIN: %s", generated_pin);
    //     ssd1306_display_text(&dev, 4, pin_buf, strlen(pin_buf), false);
    //     ssd1306_display_text(&dev, 6, "Uzyj aplikacji", 14, false);
    //     return;
    // }

    if (current_screen != last_screen)
    {
        ssd1306_clear_screen(&dev, false);
        last_screen = current_screen;
    }

    char buf[32];
    switch (current_screen)
    {
    case 0: // CZAS
        time_t now;
        struct tm ti;
        time(&now);
        localtime_r(&now, &ti);
        strftime(buf, sizeof(buf), "%H:%M:%S", &ti);
        ssd1306_display_text(&dev, 1, "    GODZINA", 11, false);
        ssd1306_display_text(&dev, 3, buf, strlen(buf), true);
        EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
        if (!(bits & WIFI_CONNECTED_BIT))
            ssd1306_display_text(&dev, 0, " [ BRAK WIFI ] ", 15, true);
        else
            ssd1306_display_text(&dev, 0, "   POLACZONO   ", 15, false);
        snprintf(buf, sizeof(buf), "%-16.16s", current_display_text);
        ssd1306_display_text(&dev, 6, buf, 16, false);
        break;

    case 1: // POGODA
        ssd1306_display_text(&dev, 0, "--- POGODA ---", 14, false);
        snprintf(buf, sizeof(buf), "Temp: %.1f C   ", g_temp); // Spacje na końcu czyszczą resztę linii
        ssd1306_display_text(&dev, 2, buf, strlen(buf), false);
        snprintf(buf, sizeof(buf), "Wilg: %.1f %%   ", g_hum);
        ssd1306_display_text(&dev, 4, buf, strlen(buf), false);
        snprintf(buf, sizeof(buf), "Cisn: %.0f hPa  ", g_press);
        ssd1306_display_text(&dev, 6, buf, strlen(buf), false);
        break;

    case 2: // SWIATLO
        ssd1306_display_text(&dev, 0, "--- SWIATLO ---", 15, false);
        snprintf(buf, sizeof(buf), "Lux: %.0f      ", g_lux);
        ssd1306_display_text(&dev, 2, buf, strlen(buf), false);

        if (g_lux > 300)
            ssd1306_display_text(&dev, 5, "ZA JASNO!   ", 12, true);
        else if (g_lux < 50)
            ssd1306_display_text(&dev, 5, "ZA CIEMNO... ", 12, false);
        else
            ssd1306_display_text(&dev, 5, "Idealnie!    ", 12, false);
        break;

    case 3: // KOTEK
        // Tutaj blink musi czyścić linię, żeby uszy nie zostawały
        if (cat_blink)
            ssd1306_display_text(&dev, 1, "   (=^~~^=)   ", 14, false);
        else
            ssd1306_display_text(&dev, 1, "   (=^..^=)   ", 14, false);

        ssd1306_display_text(&dev, 4, "Jestes piekna!", 15, false);
        ssd1306_display_text(&dev, 6, "  MILEGO DNIA!", 14, false);
        break;
    }
}

// --- TASKI SYSTEMOWE ---

void system_timer_task(void *pvParameters)
{
    while (1)
    {
        if (mirror_timer > 0)
        {
            mirror_timer--;
            if (mirror_timer == 0)
            {
                if (is_mirror_on)
                { // Wykonaj tylko jeśli było włączone
                    is_mirror_on = false;
                    // --- ZMIANA: Graj "Do widzenia" przy auto-wyłączeniu ---
                    send_dfplayer_cmd(0x12, 3);
                    refresh_oled();
                }
            }
        }
        if (lockout_timer > 0)
            lockout_timer--;
        if (music_11s_timer > 0)
        {
            music_11s_timer--;
            if (music_11s_timer == 0)
                send_dfplayer_cmd(0x16, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void button_task(void *pvParameters)
{
    touch_pad_init();
    touch_pad_config(T_SCREEN_CH, 0);
    touch_pad_config(T_POWER_CH, 0);
    touch_pad_config(T_PARTY_CH, 0);
    uint16_t v1, v2, v3;
    int pwr_hold = 0;

    while (1)
    {
        touch_pad_read(T_SCREEN_CH, &v1);
        touch_pad_read(T_POWER_CH, &v2);
        touch_pad_read(T_PARTY_CH, &v3);

        // Przycisk 1: Zmiana ekranu
        if (v1 < TOUCH_THRESH)
        {
            current_screen = (current_screen + 1) % 4;
            refresh_oled();
            vTaskDelay(pdMS_TO_TICKS(300));
        }

        // Przycisk 2: Power (Hold)
        if (v2 < TOUCH_THRESH)
        {
            pwr_hold++;
            if (pwr_hold == 20)
            { // ok 2 sekundy trzymania
                if (is_mirror_on)
                {
                    is_mirror_on = false;
                    mirror_timer = 0;
                    lockout_timer = pir_lockout_duration;
                    send_dfplayer_cmd(0x12, 3); // Pauza
                }
                else
                {
                    is_mirror_on = true;
                    mirror_timer = default_mirror_timeout;
                    lockout_timer = 0;
                    send_dfplayer_cmd(0x12, 2); // Start play
                }
                refresh_oled();
            }
        }
        else
        {
            pwr_hold = 0;
        }

        // Kabelek 3: Party Mode
        if (v3 < TOUCH_THRESH)
        {
            is_party_mode = !is_party_mode;
            if (is_party_mode)
            {
                uint16_t songs[] = {4, 5, 6}; // Tylko te trzy numery
                int idx = esp_random() % 3;
                send_dfplayer_cmd(0x12, songs[idx]); // Losuje 4, 5 lub 6
            }
            else
            {
                send_dfplayer_cmd(0x16, 0); // Stop
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        // Mruganie kota
        if (current_screen == 3 && is_mirror_on)
        {
            static int bc = 0;
            if (++bc > 20)
            {
                cat_blink = !cat_blink;
                refresh_oled();
                bc = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// --- ODCZYT SENSORÓW (Rzeczywisty) ---
void telemetry_task(void *pvParameters)
{
    bme280_init(I2C_PORT_NUM, BME_ADDR);

    // Licznik do wysyłania MQTT rzadziej niż sprawdzania czujników
    static int mqtt_send_counter = 0;

    while (1)
    {
        // 1. Odczyt BME280 (Szybki odczyt co 1s)
        if (bme280_read_float_data(I2C_PORT_NUM, BME_ADDR, &g_temp, &g_press, &g_hum) != ESP_OK)
        {
            // Obsługa błędu (opcjonalna)
        }

        // 2. Odczyt BH1750 (To trwa ok 180ms, więc jest bezpieczne co 1s)
        uint8_t cmd = 0x10;
        i2c_master_write_to_device(I2C_PORT_NUM, BH1750_ADDR, &cmd, 1, 100 / portTICK_PERIOD_MS);
        vTaskDelay(pdMS_TO_TICKS(180)); // Czekamy na pomiar

        uint8_t d[2];
        if (i2c_master_read_from_device(I2C_PORT_NUM, BH1750_ADDR, d, 2, 100 / portTICK_PERIOD_MS) == ESP_OK)
        {
            g_lux = ((d[0] << 8) | d[1]) / 1.2;
        }

        // 3. Logika PIR (Ruch)
        if (gpio_get_level(PIR_PIN) && lockout_timer == 0)
        {
            if (!is_mirror_on)
            {
                is_mirror_on = true;
                // --- NOWOŚĆ: Graj "Dzień dobry" przy wykryciu osoby ---
                send_dfplayer_cmd(0x12, 2);
                ESP_LOGI(TAG, "Auto-wybudzenie: Gra 0002.mp3");
            }
            // Zawsze resetuj licznik do wartości domyślnej, gdy jest ruch
            mirror_timer = default_mirror_timeout;
        }

        // 4. Logika Muzyki (Teraz bardzo responsywna!)
        if (g_lux > 600.0)
        {
            // Jeśli jasno I jeszcze nie graliśmy w tej sesji jasności I timer jest 0
            if (music_11s_timer == 0 && !lux_music_played)
            {
                send_dfplayer_cmd(0x12, 1); // Graj
                music_11s_timer = 11;       // Ustaw timer na 11s
                lux_music_played = true;    // Zablokuj ponowne granie
                ESP_LOGI(TAG, "Za jasno! Start Skolima na 11s");
            }
        }
        else if (g_lux < 550.0)
        {
            // RESET: Jeśli światło spadnie, odblokuj możliwość grania
            if (lux_music_played)
            {
                lux_music_played = false;
                ESP_LOGI(TAG, "Ciemno - reset blokady muzyki");
            }
        }

        // 5. Wysyłanie MQTT (Tylko co 5. obrót pętli = co 5 sekund)
        mqtt_send_counter++;
        if (mqtt_send_counter >= 5)
        {
            mqtt_send_counter = 0; // Reset licznika

            if (xEventGroupGetBits(s_wifi_event_group) & MQTT_CONNECTED_BIT)
            {
                char p[256];
                snprintf(p, sizeof(p),
                         "{\"temp\": %.1f, \"hum\": %.1f, \"lux\": %.1f, \"press\": %.1f, \"time\": %d}",
                         g_temp, g_hum, g_lux, g_press, mirror_timer);

                char pub_topic[128];
                snprintf(pub_topic, sizeof(pub_topic), "%s/%s/telemetry", TOPIC_ROOT, esp_mac_str);

                esp_mqtt_client_publish(client, pub_topic, p, 0, 0, 0);
                ESP_LOGI(TAG, "Wysłano telemetrię: %s", p);
            }
        }

        refresh_oled();

        // ZMIANA: Czekamy tylko 1 sekundę, a nie 5!
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// --- ODBIÓR KOMEND Z SERWERA ---
void handle_command_json(const char *json_str)
{
    ESP_LOGI(TAG, "Odebrano JSON: %s", json_str);

    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL)
        return;

    cJSON *action = cJSON_GetObjectItem(root, "action");
    if (cJSON_IsString(action))
    {

        // 1. Obsługa configure_all (tekst, jasność, głośność)
        if (strcmp(action->valuestring, "configure_all") == 0)
        {
            cJSON *txt = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(txt))
            {
                snprintf(current_display_text, sizeof(current_display_text), "%s", txt->valuestring);

                // --- LOGIKA UNPAIR ---
                // Jeśli tekst to "Ready to pair", czyścimy WiFi i restartujemy
                // if (strcmp(txt->valuestring, "Ready to pair") == 0) {
                //     ESP_LOGW(TAG, "Otrzymano sygnał Unpair! Czyszczenie WiFi...");
                //     esp_wifi_restore(); // Kasuje dane WiFi z pamięci NVS
                //     esp_restart();      // Restartuje urządzenie
                // }
                if (strcmp(txt->valuestring, "Ready to pair") == 0)
                {
                    ESP_LOGW(TAG, "Otrzymano sygnał Unpair! Czyszczenie NVS...");

                    // ZAMIAST esp_wifi_restore() UŻYJ TEGO:
                    wifi_ble_force_erase_credentials();

                    // Daj chwilę na zapisanie zmian we flashu
                    vTaskDelay(pdMS_TO_TICKS(1000));

                    esp_restart(); // Restartuje urządzenie
                }
            }

            cJSON *lgt = cJSON_GetObjectItem(root, "light");
            if (cJSON_IsNumber(lgt))
                set_led_brightness(lgt->valueint);

            cJSON *vol = cJSON_GetObjectItem(root, "volume");
            if (cJSON_IsNumber(vol))
                send_dfplayer_cmd(0x06, (uint16_t)vol->valueint);
        }

        // 2. Obsługa set_timer
        else if (strcmp(action->valuestring, "set_timer") == 0 || strcmp(action->valuestring, "set_timers") == 0)
        {
            // Zmieniamy nazwy kluczy na zgodne z odebranym JSONem
            cJSON *timeout_val = cJSON_GetObjectItem(root, "on_time");
            cJSON *lockout_val = cJSON_GetObjectItem(root, "lock_time");

            if (cJSON_IsNumber(timeout_val))
            {
                default_mirror_timeout = timeout_val->valueint;
                if (is_mirror_on)
                {
                    mirror_timer = default_mirror_timeout;
                }
                ESP_LOGI(TAG, "Ustawiono timeout świecenia (on_time): %d", default_mirror_timeout);
            }
            else
            {
                ESP_LOGW(TAG, "Nie znaleziono pola 'on_time' lub nie jest liczbą");
            }

            if (cJSON_IsNumber(lockout_val))
            {
                pir_lockout_duration = lockout_val->valueint;
                ESP_LOGI(TAG, "Ustawiono blokadę PIR (lock_time): %d", pir_lockout_duration);
            }
            else
            {
                ESP_LOGW(TAG, "Nie znaleziono pola 'lock_time' lub nie jest liczbą");
            }
        }
    }
    cJSON_Delete(root);
    refresh_oled();
}

// --- SETUP SIECI (Poprawiony - usunięto podwójną inicjalizację) ---
// static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
// {
//     if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
//     {
//         esp_wifi_connect();
//     }
//     else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
//     {
//         xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | MQTT_CONNECTED_BIT);
//         esp_wifi_connect();
//     }
//     else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
//     {
//         ESP_LOGI(TAG, "Otrzymano IP!");
//         xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

//         // Start MQTT po uzyskaniu IP
//         static bool mqtt_started = false;
//         if (!mqtt_started)
//         {
//             mqtt_app_start();
//             mqtt_started = true;
//         }
//     }
// }

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = data;
    switch (id)
    {
    case MQTT_EVENT_CONNECTED:
        xEventGroupSetBits(s_wifi_event_group, MQTT_CONNECTED_BIT);
        char sub_topic[128];
        snprintf(sub_topic, sizeof(sub_topic), "%s/%s/cmd", TOPIC_ROOT, esp_mac_str);
        esp_mqtt_client_subscribe(client, sub_topic, 0);
        break;

    case MQTT_EVENT_DISCONNECTED:
        // Czyścimy bit MQTT, żeby telemetria przestała próbować wysyłać dane
        xEventGroupClearBits(s_wifi_event_group, MQTT_CONNECTED_BIT);
        ESP_LOGW(TAG, "MQTT Rozłączone!");
        break;

    case MQTT_EVENT_DATA:
        char *b = malloc(e->data_len + 1);
        memcpy(b, e->data, e->data_len);
        b[e->data_len] = 0;
        handle_command_json(b);
        free(b);
        break;
    default:
        break;
    }
}

// void wifi_init_sta(void) {
//     s_wifi_event_group = xEventGroupCreate();

//     // 1. Inicjalizacja bazowa WiFi (Musi być przed managerem!)
//     esp_netif_create_default_wifi_sta();
//     // SoftAP jest potrzebny do parowania
//     esp_netif_create_default_wifi_ap();

//     wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
//     ESP_ERROR_CHECK(esp_wifi_init(&cfg));

//     // 2. Generowanie PINu (MAC)
//     uint8_t mac[6];
//     esp_read_mac(mac, ESP_MAC_WIFI_STA);
//     snprintf(generated_pin, sizeof(generated_pin), "%02X%02X%02X%02X", mac[2], mac[3], mac[4], mac[5]);

//     // 3. Konfiguracja managera
//     wifi_prov_mgr_config_t config = {
//         .scheme = wifi_prov_scheme_softap,
//         .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
//         .app_event_handler = WIFI_PROV_EVENT_HANDLER_NONE
//     };

//     ESP_ERROR_CHECK(wifi_prov_mgr_init(config));

//     // 4. Rejestracja eventów
//     esp_event_handler_instance_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &provisioning_event_handler, NULL, NULL);
//     esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
//     esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

//     // 5. Sprawdzenie stanu parowania
//     bool provisioned = false;
//     wifi_prov_mgr_is_provisioned(&provisioned);

//     if (!provisioned) {
//         ESP_LOGI(TAG, "STARTUJE TRYB PAROWANIA... PIN: %s", generated_pin);
//         is_provisioning = true;
//         refresh_oled();

//         char service_name[] = "PROV_LUSTERKO";
//         ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, generated_pin, service_name, NULL));
//     } else {
//         ESP_LOGI(TAG, "URZADZENIE SPAROWANE. LACZE...");
//         is_provisioning = false;
//         wifi_prov_mgr_deinit();

//         esp_wifi_set_mode(WIFI_MODE_STA);
//         esp_wifi_start();
//     }
// }

void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mc = {.broker.address.uri = ESP_MQTT_BROKER_URL};
    client = esp_mqtt_client_init(&mc);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL); // Upewnij się że masz mqtt_event_handler zdefiniowany
    esp_mqtt_client_start(client);
}

void start_mqtt_handler(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(esp_mac_str, sizeof(esp_mac_str), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // 1. Inicjalizacja hardware
    ledc_timer_config_t lt = {.speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = LEDC_TIMER_0, .duty_resolution = LEDC_TIMER_13_BIT, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK};
    ledc_timer_config(&lt);
    ledc_channel_config_t lc = {.speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .intr_type = LEDC_INTR_DISABLE, .gpio_num = LED_PIN, .duty = 0};
    ledc_channel_config(&lc);
    gpio_set_direction(PIR_PIN, GPIO_MODE_INPUT);

    // 2. Start Tasków (OLED i czujniki)
    xTaskCreate(button_task, "button", 4096, NULL, 10, NULL);
    xTaskCreate(telemetry_task, "telemetry", 4096, NULL, 5, NULL);
    xTaskCreate(system_timer_task, "sys_timer", 2048, NULL, 5, NULL);

    // 3. Start parowania/WiFi (MQTT wystartuje samo przez event handler)
    // wifi_init_sta();
    s_wifi_event_group = xEventGroupCreate();
}