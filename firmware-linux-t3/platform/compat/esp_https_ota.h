#pragma once
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_http_client.h"
#define ESP_ERR_HTTPS_OTA_IN_PROGRESS 0x8001
/* OTA over HTTPS is an ESP concept; the T3 flashes boot.img differently.
 * Stub to "unsupported" so the update path safely no-ops for now. */
typedef void *esp_https_ota_handle_t;
typedef struct { const esp_http_client_config_t *http_config; } esp_https_ota_config_t;
static inline esp_err_t esp_https_ota_begin(const esp_https_ota_config_t *c, esp_https_ota_handle_t *h){ (void)c; if(h)*h=0; return ESP_FAIL; }
static inline esp_err_t esp_https_ota_perform(esp_https_ota_handle_t h){ (void)h; return ESP_FAIL; }
static inline int esp_https_ota_is_complete_data_received(esp_https_ota_handle_t h){ (void)h; return 0; }
static inline esp_err_t esp_https_ota_finish(esp_https_ota_handle_t h){ (void)h; return ESP_FAIL; }
static inline esp_err_t esp_https_ota_abort(esp_https_ota_handle_t h){ (void)h; return ESP_OK; }
static inline esp_err_t esp_https_ota_get_img_desc(esp_https_ota_handle_t h, esp_app_desc_t *d){ (void)h; (void)d; return ESP_FAIL; }
static inline int esp_https_ota_get_image_size(esp_https_ota_handle_t h){ (void)h; return 0; }
static inline int esp_https_ota_get_image_len_read(esp_https_ota_handle_t h){ (void)h; return 0; }
static inline esp_err_t esp_https_ota(const esp_https_ota_config_t *c){ (void)c; return ESP_FAIL; }
