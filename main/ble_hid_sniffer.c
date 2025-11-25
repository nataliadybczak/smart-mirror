#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <math.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "JABRA_RADAR"

// --- KONFIGURACJA ---
// WPISZ DOKŁADNĄ NAZWĘ SWOICH SŁUCHAWEK:
static const char remote_device_name[] = "Jabra Elite 4 Active"; 

#define PROFILE_NUM      1
#define PROFILE_A_APP_ID 0
#define INVALID_HANDLE   0

static bool connect = false;
static esp_bd_addr_t remote_bda = {0};

// Parametry skanowania
static esp_ble_scan_params_t ble_scan_params = {
    .scan_type              = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type          = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy     = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval          = 0x50,
    .scan_window            = 0x30,
    .scan_duplicate         = BLE_SCAN_DUPLICATE_DISABLE
};

struct gattc_profile_inst {
    esp_gattc_cb_t gattc_cb;
    uint16_t gattc_if;
    uint16_t app_id;
    uint16_t conn_id;
};

static void gattc_profile_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);

static struct gattc_profile_inst gl_profile_tab[PROFILE_NUM] = {
    [PROFILE_A_APP_ID] = {
        .gattc_cb = gattc_profile_event_handler,
        .gattc_if = ESP_GATT_IF_NONE,
    },
};

// --- FUNKCJA RYSUJĄCA RADAR ---
void print_radar_bar(int rssi) {
    // RSSI waha się zazwyczaj od -30 (bardzo blisko) do -100 (bardzo daleko/brak sygnału)
    // Mapujemy to na pasek o długości 20 znaków
    
    int strength = 100 + rssi; // Przesuwamy skalę (0 to -100dBm, 70 to -30dBm)
    if (strength < 0) strength = 0;
    
    int bars = strength / 3; // Skalowanie do paska
    if (bars > 20) bars = 20;

    char bar_str[22];
    memset(bar_str, ' ', 21);
    bar_str[21] = '\0';
    
    for(int i=0; i<bars; i++) bar_str[i] = '=';
    
    // Logika Alarmu
    const char* status = "OK";
    if (rssi < -85) status = "!!! ALARM - ZGUBIONO !!!";
    else if (rssi < -75) status = "Ostrzeżenie: Daleko";
    else if (rssi > -50) status = "Bardzo blisko";

    ESP_LOGI(TAG, "RSSI: %d dBm [%s] Status: %s", rssi, bar_str, status);
}

// Zadanie (Task) które co sekundę pyta o siłę sygnału
void rssi_task(void *pvParameters) {
    while(1) {
        if (connect) {
            // Zapytanie o RSSI (wynik przyjdzie w callbacku ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT)
            esp_ble_gap_read_rssi(remote_bda);
        }
        vTaskDelay(pdMS_TO_TICKS(1000)); // Odświeżanie co 1s
    }
}

static void gattc_profile_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    esp_ble_gattc_cb_param_t *p_data = (esp_ble_gattc_cb_param_t *)param;

    switch (event) {
    case ESP_GATTC_OPEN_EVT:
        if (param->open.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Błąd połączenia!");
            break;
        }
        ESP_LOGW(TAG, ">>> POŁĄCZONO ZE SŁUCHAWKAMI! URUCHAMIAM RADAR... <<<");
        gl_profile_tab[PROFILE_A_APP_ID].conn_id = p_data->open.conn_id;
        memcpy(remote_bda, p_data->open.remote_bda, sizeof(esp_bd_addr_t));
        
        // Uruchamiamy wątek odpytywania o RSSI
        xTaskCreate(rssi_task, "rssi_task", 2048, NULL, 5, NULL);
        break;

    case ESP_GATTC_DISCONNECT_EVT:
        connect = false;
        ESP_LOGE(TAG, "!!! ROZŁĄCZONO - SŁUCHAWKI POZA ZASIĘGIEM !!!");
        esp_ble_gap_start_scanning(30);
        break;
        
    default:
        break;
    }
}

static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    uint8_t *adv_name = NULL;
    uint8_t adv_name_len = 0;

    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        esp_ble_gap_start_scanning(30);
        break;
        
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            adv_name = esp_ble_resolve_adv_data(param->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_CMPL, &adv_name_len);
            if (adv_name != NULL) {
                if (strlen(remote_device_name) == adv_name_len && strncmp((char *)adv_name, remote_device_name, adv_name_len) == 0) {
                    if (connect == false) {
                        connect = true;
                        ESP_LOGI(TAG, "Znalazłem Jabrę! Łączenie...");
                        esp_ble_gap_stop_scanning();
                        esp_ble_gattc_open(gl_profile_tab[PROFILE_A_APP_ID].gattc_if, param->scan_rst.bda, BLE_ADDR_TYPE_PUBLIC, true);
                    }
                }
            }
        }
        break;
        
    case ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT:
        // Tutaj wpada wynik zapytania o RSSI
        if (param->read_rssi_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            print_radar_bar(param->read_rssi_cmpl.rssi);
        }
        break;
        
    default:
        break;
    }
}

static void esp_gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    if (event == ESP_GATTC_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) gl_profile_tab[param->reg.app_id].gattc_if = gattc_if;
    }
    int idx;
    for (idx = 0; idx < PROFILE_NUM; idx++) {
        if (gattc_if == ESP_GATT_IF_NONE || gattc_if == gl_profile_tab[idx].gattc_if) {
            if (gl_profile_tab[idx].gattc_cb) gl_profile_tab[idx].gattc_cb(event, gattc_if, param);
        }
    }
}

void start_sniffer(void)
{
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(esp_gap_cb));
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(esp_gattc_cb));
    ESP_ERROR_CHECK(esp_ble_gattc_app_register(PROFILE_A_APP_ID));
    ESP_ERROR_CHECK(esp_ble_gatt_set_local_mtu(500));
    
    ESP_ERROR_CHECK(esp_ble_gap_set_scan_params(&ble_scan_params));
}