#ifndef WIFI_BLE_MANAGER_H
#define WIFI_BLE_MANAGER_H
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
extern EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define MQTT_CONNECTED_BIT BIT2

void wifi_ble_init(void);
void wifi_ble_force_erase_credentials(void);

#endif