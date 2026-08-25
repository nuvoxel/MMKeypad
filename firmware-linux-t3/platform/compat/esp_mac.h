#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef enum { ESP_MAC_WIFI_STA, ESP_MAC_WIFI_SOFTAP, ESP_MAC_BT,
               ESP_MAC_ETH, ESP_MAC_BASE, ESP_MAC_EFUSE_FACTORY } esp_mac_type_t;
/* reads the device MAC (eth0) into mac[6] */
esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type);
