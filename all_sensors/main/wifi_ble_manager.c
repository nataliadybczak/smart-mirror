/*
 * ESP32 - Wi-Fi config over BLE (custom GATT + NVS)
 *
 * - BLE GATT serwer:
 *      Service UUID:         0x00FF
 *      Char SSID UUID:       0xFF01 (READ/WRITE)
 *      Char PASS UUID:       0xFF02 (READ/WRITE)
 */

#include <stdio.h>
#include <string.h>
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"
#include "esp_bt_main.h"
#include "mqtt_handler.h"
#include "esp_mac.h"

extern void mqtt_app_start(void);

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static void trim(char *str)
{
    int len = strlen(str);
    while (len > 0 &&
           (str[len - 1] == ' ' || str[len - 1] == '\n' || str[len - 1] == '\r'))
    {
        str[len - 1] = '\0';
        len--;
    }
}

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "driver/gpio.h"

/* BLE */
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"
#include "esp_bt_main.h"

static void nvs_erase_wifi_credentials(void);
static void button_task(void *arg);

static const char *TAG = "WIFI_BLE_CFG";

/* ==========================  TIMEOUT  ==================================== */
#define WIFI_CONNECT_TIMEOUT_MS 20000 // 15 sekund
static int64_t wifi_connect_start_time = 0;

static int64_t ble_start_time = 0;
#define BLE_TIMEOUT_MS 900000 // 60 sekund
static bool ble_active = false;

/* ==========================  GPIO / BUTTON  =============================== */

#define BUTTON_GPIO GPIO_NUM_0

#define SHORT_PRESS_MS 1000
#define LONG_PRESS_MS 3000

static void button_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE};
    gpio_config(&io_conf);
}

static bool button_pressed_at_boot(void)
{
    return (gpio_get_level(BUTTON_GPIO) == 0);
}

// ========================== ENCRIPTION / DECRYPTION  ============================ */
// #define XOR_KEY 0x5A
#define XOR_KEY 0xA7
static void xor_crypt(uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        buf[i] ^= XOR_KEY;
    }
}

static uint8_t ssid_enc[32];
static uint8_t pass_enc[64];
static uint16_t ssid_enc_len = 0;
static uint16_t pass_enc_len = 0;

/* ==========================  NVS (SSID/PASS)  ============================= */

#define WIFI_NVS_NAMESPACE "wifi_cfg"
#define WIFI_NVS_KEY_SSID "ssid"
#define WIFI_NVS_KEY_PASS "pass"

static bool ssid_locked = false;

// static esp_err_t nvs_save_wifi_credentials(const char *ssid_enc, const char *pass_enc)
// {
//     nvs_handle_t nvs;
//     esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
//     if (err != ESP_OK)
//         return err;

//     err = nvs_set_str(nvs, WIFI_NVS_KEY_SSID, ssid_enc);
//     if (err != ESP_OK)
//     {
//         nvs_close(nvs);
//         return err;
//     }

//     err = nvs_set_str(nvs, WIFI_NVS_KEY_PASS, pass_enc);
//     if (err != ESP_OK)
//     {
//         nvs_close(nvs);
//         return err;
//     }

//     err = nvs_commit(nvs);
//     nvs_close(nvs);
//     return err;
// }
static esp_err_t nvs_save_wifi_credentials(const char *ssid_plain, const char *pass_plain)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK)
        return err;

    err = nvs_set_str(nvs, WIFI_NVS_KEY_SSID, ssid_plain);
    if (err != ESP_OK)
    {
        nvs_close(nvs);
        return err;
    }

    err = nvs_set_str(nvs, WIFI_NVS_KEY_PASS, pass_plain);
    if (err != ESP_OK)
    {
        nvs_close(nvs);
        return err;
    }

    err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static esp_err_t nvs_load_wifi_credentials(char *ssid_enc, size_t ssid_len,
                                           char *pass_enc, size_t pass_len,
                                           bool *have_credentials)
{
    *have_credentials = false;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK)
        return err;

    size_t s_len = ssid_len;
    size_t p_len = pass_len;

    if (nvs_get_str(nvs, WIFI_NVS_KEY_SSID, ssid_enc, &s_len) == ESP_OK &&
        nvs_get_str(nvs, WIFI_NVS_KEY_PASS, pass_enc, &p_len) == ESP_OK)
    {
        *have_credentials = true;
    }

    nvs_close(nvs);
    return ESP_OK;
}

static void nvs_erase_wifi_credentials(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK)
    {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGW(TAG, "Wi-Fi credentials erased from NVS (namespace: %s)", WIFI_NVS_NAMESPACE);
    }
    else if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGW(TAG, "Wi-Fi NVS namespace not found, nothing to erase");
    }
    else
    {
        ESP_LOGE(TAG, "nvs_open for erase failed: %s", esp_err_to_name(err));
    }
}

/* =============================  Wi-Fi  ==================================== */

static EventGroupHandle_t wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;

static bool wifi_initialized = false;
static bool wifi_started = false;
static bool wifi_connected = false;

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT)
    {
        switch (event_id)
        {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "WIFI_EVENT_STA_START -> esp_wifi_connect()");
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting...");
            wifi_connected = false;
            xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
            esp_wifi_connect();
            break;
        default:
            break;
        }
    }
    else if (event_base == IP_EVENT &&
             event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_connected = true;
        wifi_connect_start_time = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);

        ESP_LOGW(TAG, "Wi-Fi connected — disabling BLE to hide device");

        // DO BLOKOWANIA BLE PO POŁĄCZENIU WI-FI:
        esp_ble_gap_stop_advertising();

        // esp_bluedroid_disable();
        // esp_bt_controller_disable();

        ssid_locked = true;
        ESP_LOGW(TAG, "SSID LOCKED — Wi-Fi connected successfully.");

        // ESP_LOGI(TAG, "WiFi Connected! Starting MQTT...");
        // mqtt_app_start();
        static bool mqtt_was_started = false; // Zmienna pamiętająca stan

        if (!mqtt_was_started)
        {
            ESP_LOGI(TAG, "WiFi Connected! Starting MQTT for the first time...");
            mqtt_app_start();
            mqtt_was_started = true;
        }
        else
        {
            ESP_LOGI(TAG, "WiFi Reconnected! MQTT client will reconnect automatically (no need to restart).");
            // Nie wywołujemy mqtt_app_start(), biblioteka sama wznowi połączenie
        }
    }
}

static void wifi_stack_init(void)
{
    if (wifi_initialized)
        return;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_event_group = xEventGroupCreate();

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
                                               ESP_EVENT_ANY_ID,
                                               &wifi_event_handler,
                                               NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,
                                               IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler,
                                               NULL));

    wifi_initialized = true;
}

// static void wifi_start_with_credentials(const char *ssid, const char *pass)
// {
//     if (!wifi_initialized)
//     {
//         wifi_stack_init();
//     }

//     wifi_config_t wifi_cfg = {0};
//     strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
//     strncpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password) - 1);

//     wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
//     wifi_cfg.sta.pmf_cfg.capable = true;
//     wifi_cfg.sta.pmf_cfg.required = false;

//     ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
//     ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));

//     if (!wifi_started)
//     {
//         ESP_ERROR_CHECK(esp_wifi_start());
//         wifi_started = true;
//     }
//     else
//     {
//         esp_wifi_disconnect();
//     }

//     ESP_LOGI(TAG, "Connecting to Wi-Fi: SSID=\"%s\"", ssid);
//     wifi_connect_start_time = esp_timer_get_time() / 1000; // ms

//     // ESP_ERROR_CHECK(esp_wifi_connect());
// }
static void wifi_start_with_credentials(const char *ssid, const char *pass)
{
    if (!wifi_initialized)
    {
        wifi_stack_init();
    }

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password) - 1);

    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));

    if (!wifi_started)
    {
        ESP_ERROR_CHECK(esp_wifi_start());
        wifi_started = true;
    }

    // ESP_LOGI(TAG, "Connecting to Wi-Fi: SSID=\"%s\"", ssid);
    // wifi_connect_start_time = esp_timer_get_time() / 1000;

    // ESP_ERROR_CHECK(esp_wifi_connect());
    ESP_LOGI(TAG, "Connecting to Wi-Fi: SSID=\"%s\"", ssid);
    wifi_connect_start_time = esp_timer_get_time() / 1000;

    // Usuwamy ESP_ERROR_CHECK. Jeśli zwróci błąd, to znaczy że już się łączy
    // (przez Event Handler), więc ignorujemy to.
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "esp_wifi_connect failed (probably already connecting): %s", esp_err_to_name(err));
    }
}

/* Reklama BLE */
static esp_ble_adv_params_t adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x40,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static esp_ble_adv_data_t adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = 0,
    .p_service_uuid = NULL,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

/* ==============================  BLE  ===================================== */

#define GATTS_TAG "GATTS_WIFI_CFG"

// #define DEVICE_NAME "ESP32_WIFI_CFG"
static char ble_name[32];

#define GATTS_SERVICE_UUID_TEST 0x00FF
#define GATTS_CHAR_UUID_SSID 0xFF01
#define GATTS_CHAR_UUID_PASS 0xFF02

#define GATTS_NUM_HANDLE 8

static uint16_t gatts_service_handle = 0;
static esp_gatt_srvc_id_t gatts_service_id;

static uint16_t ssid_char_handle = 0;
static uint16_t pass_char_handle = 0;

static esp_gatt_if_t gatts_if_global = 0;

static uint8_t ssid_value[32] = {0};
static uint8_t pass_value[64] = {0};
static uint16_t ssid_value_len = 0;
static uint16_t pass_value_len = 0;

static void gap_event_handler(esp_gap_ble_cb_event_t event,
                              esp_ble_gap_cb_param_t *param)
{
    switch (event)
    {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        ESP_LOGI(GATTS_TAG, "Adv data set, starting advertising");
        esp_ble_gap_start_advertising(&adv_params);
        ble_start_time = esp_timer_get_time() / 1000;
        ble_active = true;
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS)
        {
            ESP_LOGE(GATTS_TAG, "Advertising start failed");
        }
        break;
    default:
        break;
    }
}

static void gatts_event_handler(esp_gatts_cb_event_t event,
                                esp_gatt_if_t gatts_if,
                                esp_ble_gatts_cb_param_t *param)
{
    switch (event)
    {
    case ESP_GATTS_REG_EVT:
    {
        ESP_LOGI(GATTS_TAG, "ESP_GATTS_REG_EVT, status %d, app_id %d",
                 param->reg.status, param->reg.app_id);

        gatts_if_global = gatts_if;

        // esp_err_t set_dev_name_ret = esp_ble_gap_set_device_name(DEVICE_NAME);
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);

        snprintf(
            ble_name,
            sizeof(ble_name),
            "SmartMirror_%02X%02X%02X%02X%02X%02X",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        ESP_LOGI(GATTS_TAG, "BLE NAME = %s", ble_name);

        esp_err_t set_dev_name_ret = esp_ble_gap_set_device_name(ble_name);
        if (set_dev_name_ret)
        {
            ESP_LOGE(GATTS_TAG, "set device name failed, error code = %x", set_dev_name_ret);
        }

        esp_err_t ret = esp_ble_gap_config_adv_data(&adv_data);
        if (ret)
        {
            ESP_LOGE(GATTS_TAG, "config adv data failed, error code = %x", ret);
        }

        gatts_service_id.is_primary = true;
        gatts_service_id.id.inst_id = 0x00;
        gatts_service_id.id.uuid.len = ESP_UUID_LEN_16;
        gatts_service_id.id.uuid.uuid.uuid16 = GATTS_SERVICE_UUID_TEST;

        esp_ble_gatts_create_service(gatts_if,
                                     &gatts_service_id,
                                     GATTS_NUM_HANDLE);
        break;
    }

    case ESP_GATTS_CREATE_EVT:
    {
        ESP_LOGI(GATTS_TAG, "SERVICE_CREATE_EVT, status %d, service_handle %d",
                 param->create.status, param->create.service_handle);

        gatts_service_handle = param->create.service_handle;
        esp_ble_gatts_start_service(gatts_service_handle);

        esp_bt_uuid_t char_uuid;
        char_uuid.len = ESP_UUID_LEN_16;
        char_uuid.uuid.uuid16 = GATTS_CHAR_UUID_SSID;

        esp_gatt_char_prop_t prop = ESP_GATT_CHAR_PROP_BIT_READ |
                                    ESP_GATT_CHAR_PROP_BIT_WRITE;

        esp_err_t add_char_ret = esp_ble_gatts_add_char(
            gatts_service_handle,
            &char_uuid,
            ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
            prop,
            NULL, NULL);
        if (add_char_ret)
        {
            ESP_LOGE(GATTS_TAG, "add char SSID failed, error code = %x", add_char_ret);
        }

        break;
    }

    case ESP_GATTS_ADD_CHAR_EVT:
    {
        ESP_LOGI(GATTS_TAG, "ADD_CHAR_EVT, uuid=0x%04x, handle=%d",
                 param->add_char.char_uuid.uuid.uuid16,
                 param->add_char.attr_handle);

        uint16_t uuid = param->add_char.char_uuid.uuid.uuid16;

        if (uuid == GATTS_CHAR_UUID_SSID)
        {
            ssid_char_handle = param->add_char.attr_handle;

            esp_bt_uuid_t char_uuid;
            char_uuid.len = ESP_UUID_LEN_16;
            char_uuid.uuid.uuid16 = GATTS_CHAR_UUID_PASS;

            esp_gatt_char_prop_t prop = ESP_GATT_CHAR_PROP_BIT_READ |
                                        ESP_GATT_CHAR_PROP_BIT_WRITE;

            esp_err_t add_char_ret = esp_ble_gatts_add_char(
                gatts_service_handle,
                &char_uuid,
                ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                prop,
                NULL, NULL);
            if (add_char_ret)
            {
                ESP_LOGE(GATTS_TAG, "add char PASS failed, error code = %x", add_char_ret);
            }
        }
        else if (uuid == GATTS_CHAR_UUID_PASS)
        {
            pass_char_handle = param->add_char.attr_handle;
        }
        break;
    }

    //
    case ESP_GATTS_READ_EVT:
    {
        esp_gatt_rsp_t rsp = {0};
        rsp.attr_value.handle = param->read.handle;

        if (param->read.handle == ssid_char_handle)
        {
            // rsp.attr_value.len = ssid_value_len;
            // memcpy(rsp.attr_value.value, ssid_value, ssid_value_len); // encrypted
            rsp.attr_value.len = ssid_enc_len;
            memcpy(rsp.attr_value.value, ssid_enc, ssid_enc_len);
        }
        else if (param->read.handle == pass_char_handle)
        {
            // rsp.attr_value.len = pass_value_len;
            // memcpy(rsp.attr_value.value, pass_value, pass_value_len); // encrypted
            rsp.attr_value.len = pass_enc_len;
            memcpy(rsp.attr_value.value, pass_enc, pass_enc_len);
        }
        else
        {
            rsp.attr_value.len = 0;
        }

        esp_ble_gatts_send_response(gatts_if,
                                    param->read.conn_id,
                                    param->read.trans_id,
                                    ESP_GATT_OK,
                                    &rsp);
        break;
    }

    case ESP_GATTS_WRITE_EVT:
    {
        ESP_LOGI(GATTS_TAG, "WRITE_EVT: handle=%d len=%d",
                 param->write.handle, param->write.len);

        if (!param->write.is_prep)
        {

            /* --- SSID WRITE --- */
            if (param->write.handle == ssid_char_handle)
            {
                if (ssid_locked)
                {
                    ESP_LOGW(TAG, "SSID WRITE IGNORED — SSID is LOCKED");
                    break; // <-- teraz poprawnie ignoruje SSID
                }

                // Jeśli w przyszłości odblokujesz — to działa:
                ssid_value_len = MIN(param->write.len, sizeof(ssid_value) - 1);
                memcpy(ssid_value, param->write.value, ssid_value_len);
                ssid_value[ssid_value_len] = '\0';

                trim((char *)ssid_value);                    // Usuń spacje z końca
                ssid_value_len = strlen((char *)ssid_value); // Zaktualizuj długość

                memcpy(ssid_enc, ssid_value, ssid_value_len + 1);
                xor_crypt(ssid_enc, ssid_value_len);
                // ssid_enc_len = ssid_value_len;
                // esp_ble_gatts_set_attr_value(ssid_char_handle, ssid_enc_len, ssid_enc);
                ssid_enc_len = ssid_value_len;

                if (ssid_enc_len > 0)
                {
                    esp_ble_gatts_set_attr_value(ssid_char_handle, ssid_enc_len, ssid_enc);
                }
                else
                {
                    ESP_LOGW(TAG, "SSID empty -> not setting attr value");
                }

                ESP_LOGI(TAG, "SSID ENCRYPTED FOR READ = %.*s", ssid_enc_len, ssid_enc);

                // restart oczekiwania na hasło
                pass_value_len = 0;
            }

            /* --- PASSWORD WRITE --- */
            else if (param->write.handle == pass_char_handle)
            {
                ESP_LOGW(TAG, "PASSWORD WRITE, ssid_len=%d", ssid_value_len);

                // pass_value_len = MIN(param->write.len, sizeof(pass_value) - 1);
                // memcpy(pass_value, param->write.value, pass_value_len);

                // pass_value[pass_value_len] = '\0';
                // trim((char *)pass_value);

                // ESP_LOGI(TAG, "PASS decrypted (len=%d)", pass_value_len);
                pass_value_len = MIN(param->write.len, sizeof(pass_value) - 1);
                memcpy(pass_value, param->write.value, pass_value_len);
                pass_value[pass_value_len] = '\0';

                trim((char *)pass_value);                    // Usuń spacje z końca
                pass_value_len = strlen((char *)pass_value); // Zaktualizuj długość

                // 🔥 utwórz zaszyfrowaną wersję
                memcpy(pass_enc, pass_value, pass_value_len + 1);
                xor_crypt(pass_enc, pass_value_len);
                // pass_enc_len = pass_value_len;
                // esp_ble_gatts_set_attr_value(
                //     pass_char_handle,
                //     pass_enc_len,
                //     pass_enc);
                pass_enc_len = pass_value_len;

                if (pass_enc_len > 0)
                {
                    esp_ble_gatts_set_attr_value(pass_char_handle, pass_enc_len, pass_enc);
                }
                else
                {
                    ESP_LOGW(TAG, "PASS empty -> not setting attr value");
                }
                ESP_LOGI(TAG, "PASS ENCRYPTED FOR READ = %.*s", pass_enc_len, pass_enc);
            }

            /* --- Odpowiedź BLE --- */
            if (param->write.need_rsp)
            {
                esp_ble_gatts_send_response(gatts_if,
                                            param->write.conn_id,
                                            param->write.trans_id,
                                            ESP_GATT_OK,
                                            NULL);
            }
            /* --- Po obu wartościach: ZAPISZ DO NVS + POŁĄCZ Z WIFI --- */
            // if (ssid_value_len > 0 && pass_value_len > 0)
            // {
            //     ESP_LOGE(TAG, "BOTH RECEIVED! Connecting to Wi-Fi...");

            //     /* zapis do NVS w postaci zaszyfrowanej */
            //     uint8_t enc_ssid[32];
            //     uint8_t enc_pass[64];

            //     memcpy(enc_ssid, ssid_value, ssid_value_len + 1);
            //     memcpy(enc_pass, pass_value, pass_value_len + 1);

            //     xor_crypt(enc_ssid, ssid_value_len);
            //     xor_crypt(enc_pass, pass_value_len);

            //     ESP_LOGI(TAG, "Encrypted SSID (%d bytes):", ssid_value_len);
            //     for (int i = 0; i < ssid_value_len; i++)
            //     {
            //         printf("%02X ", enc_ssid[i]);
            //     }
            //     printf("\n");

            //     ESP_LOGI(TAG, "Encrypted PASS (%d bytes):", pass_value_len);
            //     for (int i = 0; i < pass_value_len; i++)
            //     {
            //         printf("%02X ", enc_pass[i]);
            //     }
            //     printf("\n");

            //     // nvs_save_wifi_credentials((char *)enc_ssid, (char *)enc_pass);

            //     // /* odszyfrowane kopie do wifi_start */
            //     // wifi_start_with_credentials((char *)ssid_value, (char *)pass_value);
            //     nvs_save_wifi_credentials((char *)ssid_value, (char *)pass_value);
            //     wifi_start_with_credentials((char *)ssid_value, (char *)pass_value);
            // }
            if (ssid_value_len > 0 && pass_value_len > 0)
            {
                ESP_LOGE(TAG, "BOTH RECEIVED! Stopping BLE & Connecting to Wi-Fi...");

                // 1. ZATRZYMAJ BLE, ABY ZWOLNIĆ ANTENĘ DLA WI-FI (Naprawia błąd wifi:Coexist)
                esp_ble_gap_stop_advertising();
                // Opcjonalnie można ubić cały kontroler, ale stop advertising zazwyczaj wystarcza:
                // esp_bluedroid_disable();

                // 2. DEBUGOWANIE HASŁA (Sprawdź w logach, czy nie ma dziwnych znaków na końcu)
                ESP_LOGI(TAG, "Checking PASS (HEX):");
                for (int i = 0; i < pass_value_len; i++)
                {
                    printf("%02X ", pass_value[i]);
                }
                printf("\n");

                /* zapis do NVS w postaci zaszyfrowanej */
                uint8_t enc_ssid[32];
                uint8_t enc_pass[64];

                memcpy(enc_ssid, ssid_value, ssid_value_len + 1);
                memcpy(enc_pass, pass_value, pass_value_len + 1);

                xor_crypt(enc_ssid, ssid_value_len);
                xor_crypt(enc_pass, pass_value_len);

                nvs_save_wifi_credentials((char *)ssid_value, (char *)pass_value);

                // Teraz, gdy BLE nie przeszkadza, Wi-Fi powinno się połączyć
                wifi_start_with_credentials((char *)ssid_value, (char *)pass_value);
            }
            // else if (pass_value_len > 0)
            // {
            //     ESP_LOGW(TAG, "New password received — updating only password");

            //     uint8_t enc_ssid[32];
            //     uint8_t enc_pass[64];

            //     memcpy(enc_ssid, ssid_value, ssid_value_len + 1);
            //     memcpy(enc_pass, pass_value, pass_value_len + 1);

            //     xor_crypt(enc_ssid, ssid_value_len);
            //     xor_crypt(enc_pass, pass_value_len);

            //     nvs_save_wifi_credentials((char *)enc_ssid, (char *)enc_pass);

            //     wifi_start_with_credentials((char *)ssid_value, (char *)pass_value);
            // }
            else if (pass_value_len > 0)
            {
                ESP_LOGW(TAG, "New password received — updating only password");

                nvs_save_wifi_credentials((char *)ssid_value, (char *)pass_value);
                wifi_start_with_credentials((char *)ssid_value, (char *)pass_value);
            }
        }
        break;
    }

    default:
        break;
    }
}

static void ble_init(void)
{
    esp_err_t ret;

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

    ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (ret)
    {
        ESP_LOGW(GATTS_TAG, "BT controller mem release classic BT failed: %s", esp_err_to_name(ret));
    }

    ret = esp_bt_controller_init(&bt_cfg);
    if (ret)
    {
        ESP_LOGE(GATTS_TAG, "BT controller init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret)
    {
        ESP_LOGE(GATTS_TAG, "BT controller enable failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_init();
    if (ret)
    {
        ESP_LOGE(GATTS_TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_enable();
    if (ret)
    {
        ESP_LOGE(GATTS_TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(0));
}

static void button_task(void *arg)
{
    while (1)
    {
        if (gpio_get_level(BUTTON_GPIO) == 0) // przycisk wciśnięty
        {
            int press_time = 0;
            while (gpio_get_level(BUTTON_GPIO) == 0)
            {
                vTaskDelay(pdMS_TO_TICKS(10));
                press_time += 10;
            }

            if (press_time > LONG_PRESS_MS)
            {
                ESP_LOGW(TAG, "LONG PRESS → factory reset Wi-Fi!");
                nvs_erase_wifi_credentials();
                esp_restart(); // restart systemu
            }
            // else if (press_time > SHORT_PRESS_MS)
            // {
            //     ESP_LOGW(TAG, "SHORT PRESS → re-enter BLE config mode");
            //     esp_ble_gap_start_advertising(&adv_params);
            //     esp_bluedroid_enable();
            //     esp_bt_controller_enable(ESP_BT_MODE_BLE);

            //     // (opcjonalnie) odłączenie Wi-Fi
            //     esp_wifi_disconnect();
            //     wifi_connected = false;

            //     ESP_LOGI(TAG, "BLE provisioning restarted!");
            // }
            // else if (press_time > SHORT_PRESS_MS)
            // {
            //     ESP_LOGW(TAG, "SHORT PRESS → password update mode");

            //     // Restart BLE
            //     esp_ble_gap_start_advertising(&adv_params);
            //     esp_bluedroid_enable();
            //     esp_bt_controller_enable(ESP_BT_MODE_BLE);

            //     // Odłącz Wi-Fi
            //     esp_wifi_disconnect();
            //     wifi_connected = false;

            //     // ⭐ SSID pozostaje LOCKED
            //     ssid_locked = true;

            //     // ⭐ Hasło do zmiany
            //     pass_value_len = 0;

            //     ESP_LOGI(TAG, "BLE ready — SSID LOCKED, waiting for NEW PASSWORD only");
            // }
            else if (press_time > SHORT_PRESS_MS)
            {
                ESP_LOGW(TAG, "SHORT PRESS → password update mode (restart BLE)");

                // 1️⃣ Wyłącz BLE jeśli było włączone
                esp_ble_gap_stop_advertising();
                esp_bluedroid_disable();
                esp_bt_controller_disable();

                // 2️⃣ Re-init BLE
                esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
                esp_bt_controller_init(&bt_cfg);
                esp_bt_controller_enable(ESP_BT_MODE_BLE);

                esp_bluedroid_init();
                esp_bluedroid_enable();

                // 3️⃣ Re-register callbacks
                esp_ble_gap_register_callback(gap_event_handler);
                esp_ble_gatts_register_callback(gatts_event_handler);
                esp_ble_gatts_app_register(0);

                // 4️⃣ Restart advertising
                esp_ble_gap_start_advertising(&adv_params);
                ble_start_time = esp_timer_get_time() / 1000;
                ble_active = true;

                // 5️⃣ Odłączenie Wi-Fi
                esp_wifi_disconnect();
                wifi_connected = false;

                // 6️⃣ SSID ma zostać zablokowane (Tylko password)
                ssid_locked = true;

                pass_value_len = 0;

                ESP_LOGI(TAG, "BLE password update mode READY — SSID LOCKED");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ==============================  MAIN  ==================================== */

// void wifi_ble_init(void)
// {
//     esp_err_t ret;

//     ret = nvs_flash_init();
//     if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
//         ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
//     {
//         ESP_ERROR_CHECK(nvs_flash_erase());
//         ESP_ERROR_CHECK(nvs_flash_init());
//     }

//     button_init();

//     if (button_pressed_at_boot())
//     {
//         ESP_LOGW(TAG, "BOOT pressed at boot -> erasing Wi-Fi credentials from NVS");
//         nvs_erase_wifi_credentials();
//     }

//     ble_init();

//     char ssid[32] = {0};
//     char pass[64] = {0};
//     bool have_credentials = false;

//     nvs_load_wifi_credentials(ssid, sizeof(ssid),
//                               pass, sizeof(pass),
//                               &have_credentials);
//     // if (have_credentials)
//     // {
//     //     ssid_locked = true;
//     //     ESP_LOGW(TAG, "SSID locked — cannot change until long-press BOOT.");
//     // }
//     // if (have_credentials)
//     // {
//     //     ESP_LOGW(TAG, "Credentials found in NVS — trying to reconnect.");
//     //     ssid_locked = false; // <--- WAŻNE
//     //     ... wifi_start_with_credentials(ssid, pass);
//     // }

//     // if (have_credentials)
//     // {
//     //     xor_crypt((uint8_t *)ssid, strlen(ssid));
//     //     xor_crypt((uint8_t *)pass, strlen(pass));

//     //     trim(ssid);
//     //     trim(pass);

//     //     ssid_value_len = strlen(ssid);
//     //     strcpy((char *)ssid_value, ssid);

//     //     pass_value_len = strlen(pass);
//     //     strcpy((char *)pass_value, pass);

//     //     wifi_start_with_credentials(ssid, pass);
//     // }
//     if (have_credentials)
//     {
//         ESP_LOGW(TAG, "Credentials found in NVS — trying to reconnect.");

//         ssid_locked = false; // ← UNLOCK SSID on boot
//                              //    (tylko do momentu udanego Wi-Fi)

//         // xor_crypt((uint8_t *)ssid, strlen(ssid));
//         // xor_crypt((uint8_t *)pass, strlen(pass));

//         trim(ssid);
//         trim(pass);

//         ssid_value_len = strlen(ssid);
//         strcpy((char *)ssid_value, ssid);

//         pass_value_len = strlen(pass);
//         strcpy((char *)pass_value, pass);

//         wifi_start_with_credentials(ssid, pass);
//     }
//     else
//     {
//         ESP_LOGW(TAG, "No Wi-Fi credentials in NVS, waiting for BLE config...");
//     }

//     xTaskCreate(button_task, "button_task", 4096, NULL, 5, NULL);

//     while (1)
//     {
//         if (ble_active)
//         {
//             int64_t now = esp_timer_get_time() / 1000;
//             if (now - ble_start_time > BLE_TIMEOUT_MS)
//             {
//                 ESP_LOGW(TAG, "BLE timeout — disabling BLE for safety");

//                 ble_active = false;
//                 esp_ble_gap_stop_advertising();
//                 esp_bluedroid_disable();
//                 esp_bt_controller_disable();
//             }
//         }

//         if (!wifi_initialized)
//         {
//             ESP_LOGI(TAG, "Main loop: Wi-Fi stack NOT initialized yet");
//         }
//         else if (!wifi_started)
//         {
//             ESP_LOGI(TAG, "Main loop: Wi-Fi stack initialized, but Wi-Fi NOT started yet");
//         }
//         else if (!wifi_connected)
//         {
//             ESP_LOGI(TAG, "Main loop: Wi-Fi NOT connected");

//             if (wifi_connect_start_time > 0)
//             {
//                 int64_t now = esp_timer_get_time() / 1000;
//                 if (now - wifi_connect_start_time > WIFI_CONNECT_TIMEOUT_MS)
//                 {
//                     ESP_LOGW(TAG, "Wi-Fi connection TIMEOUT! Restarting BLE provisioning.");
//                     ESP_LOGW(TAG, "HINT: Press LONG to full reset SSID+PASS.");

//                     wifi_connect_start_time = 0;
//                     esp_wifi_disconnect();

//                     // Restart BLE
//                     // esp_bt_controller_enable(ESP_BT_MODE_BLE);
//                     // esp_bluedroid_enable();
//                     // esp_ble_gap_start_advertising(&adv_params);
//                     // ble_start_time = esp_timer_get_time() / 1000;
//                     // ble_active = true;
//                     esp_bt_controller_enable(ESP_BT_MODE_BLE);
//                     esp_bluedroid_enable();

//                     esp_ble_gap_register_callback(gap_event_handler);
//                     esp_ble_gatts_register_callback(gatts_event_handler);
//                     esp_ble_gatts_app_register(0);

//                     esp_ble_gap_start_advertising(&adv_params);
//                     ble_start_time = esp_timer_get_time() / 1000;
//                     ble_active = true;

//                     pass_value_len = 0;

//                     ssid_locked = true;
//                 }
//             }
//         }
//         else
//         {
//             ESP_LOGI(TAG, "Main loop: Wi-Fi CONNECTED");
//         }

//         vTaskDelay(pdMS_TO_TICKS(3000));
//     }
// }
/* ==============================  MAIN  ==================================== */

void wifi_ble_init(void)
{
    esp_err_t ret;

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    button_init();

    if (button_pressed_at_boot())
    {
        ESP_LOGW(TAG, "BOOT pressed at boot -> erasing Wi-Fi credentials from NVS");
        nvs_erase_wifi_credentials();
    }

    ble_init();

    char ssid[32] = {0};
    char pass[64] = {0};
    bool have_credentials = false;

    nvs_load_wifi_credentials(ssid, sizeof(ssid),
                              pass, sizeof(pass),
                              &have_credentials);

    if (have_credentials)
    {
        ESP_LOGW(TAG, "Credentials found in NVS — trying to reconnect.");

        ssid_locked = false;

        trim(ssid);
        trim(pass);

        ssid_value_len = strlen(ssid);
        strcpy((char *)ssid_value, ssid);

        pass_value_len = strlen(pass);
        strcpy((char *)pass_value, pass);

        wifi_start_with_credentials(ssid, pass);
    }
    else
    {
        ESP_LOGW(TAG, "No Wi-Fi credentials in NVS, waiting for BLE config...");
    }

    xTaskCreate(button_task, "button_task", 4096, NULL, 5, NULL);

    while (1)
    {
        // 1. Zabezpieczenie BLE Timeout (jeśli nikt się nie połączył przez 15 min)
        if (ble_active)
        {
            int64_t now = esp_timer_get_time() / 1000;
            if (now - ble_start_time > BLE_TIMEOUT_MS)
            {
                ESP_LOGW(TAG, "BLE timeout — disabling BLE for safety");
                ble_active = false;
                esp_ble_gap_stop_advertising();
                // esp_bluedroid_disable(); // Zostawiamy włączone, żeby łatwiej wznowić
            }
        }

        // 2. Logika stanu Wi-Fi
        if (!wifi_initialized)
        {
            // Czekamy na init
        }
        else if (!wifi_connected)
        {
            // Jeśli wystartowaliśmy łączenie (mamy credentials), ale nie ma połączenia
            if (wifi_connect_start_time > 0)
            {
                int64_t now = esp_timer_get_time() / 1000;

                // --- TU JEST KLUCZOWA POPRAWKA ---
                // Jeśli minęło 20 sekund i nadal brak Wi-Fi (np. złe hasło):
                if (now - wifi_connect_start_time > WIFI_CONNECT_TIMEOUT_MS)
                {
                    ESP_LOGE(TAG, "Wi-Fi connection TIMEOUT (Wrong password?) -> RESETTING CONFIG!");

                    // A. Zatrzymaj próby łączenia
                    esp_wifi_disconnect();
                    esp_wifi_stop();
                    wifi_started = false;
                    wifi_connect_start_time = 0; // Reset licznika

                    // B. Usuń BŁĘDNE dane z NVS (żeby po resecie nie próbował znowu)
                    // nvs_erase_wifi_credentials();

                    // C. Wyczyść bufory w pamięci RAM
                    memset(ssid_value, 0, sizeof(ssid_value));
                    memset(pass_value, 0, sizeof(pass_value));
                    ssid_value_len = 0;
                    pass_value_len = 0;

                    // D. Odblokuj możliwość wpisania nowego SSID
                    ssid_locked = false;

                    // E. RESTART BLE (Żeby użytkownik mógł spróbować ponownie)
                    ESP_LOGW(TAG, "Restarting BLE advertising...");

                    // Upewnij się, że stack jest aktywny
                    esp_bt_controller_enable(ESP_BT_MODE_BLE);
                    esp_bluedroid_enable();

                    // Zarejestruj ponownie callbacki (dla pewności, choć zwykle nie trzeba)
                    esp_ble_gap_register_callback(gap_event_handler);
                    esp_ble_gatts_register_callback(gatts_event_handler);
                    esp_ble_gatts_app_register(0);

                    // Start reklamowania
                    esp_ble_gap_start_advertising(&adv_params);

                    ble_start_time = esp_timer_get_time() / 1000;
                    ble_active = true;

                    ESP_LOGI(TAG, "SYSTEM READY FOR NEW CONFIGURATION via BLE");
                }
            }
        }
        else
        {
            // Połączono pomyślnie
            // ESP_LOGI(TAG, "Wi-Fi Heartbeat: Connected");
        }

        vTaskDelay(pdMS_TO_TICKS(1000)); // Sprawdzaj co sekundę
    }
}

// Publiczna funkcja do wywołania z mqtt_handler.c
void wifi_ble_force_erase_credentials(void)
{
    ESP_LOGW(TAG, "EXTERNAL REQUEST: Erasing Wi-Fi credentials from NVS...");
    nvs_erase_wifi_credentials(); // Wywołuje istniejącą, statyczną funkcję
}