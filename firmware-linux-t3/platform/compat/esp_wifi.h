#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "esp_mac.h"
/* no WiFi on the T3 Linux target -> fail so callers fall back to eth MAC */
static inline esp_err_t esp_wifi_get_mac(int ifx, uint8_t *mac){ (void)ifx; (void)mac; return ESP_FAIL; }
