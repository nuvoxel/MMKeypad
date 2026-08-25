/* Minimal NVS -> flat-file key/value shim for config.c. Stores the handful
 * of small keypad settings as "key=value" lines in /data/mmkeypad.conf. */
#pragma once
#include <stdint.h>
#include "esp_err.h"

typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
typedef struct nvs_store *nvs_handle_t;

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out);
void nvs_close(nvs_handle_t h);
esp_err_t nvs_commit(nvs_handle_t h);

esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out);
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t val);
esp_err_t nvs_get_u16(nvs_handle_t h, const char *key, uint16_t *out);
esp_err_t nvs_set_u16(nvs_handle_t h, const char *key, uint16_t val);
