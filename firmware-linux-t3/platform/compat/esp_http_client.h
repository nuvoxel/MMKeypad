#pragma once
#include "esp_err.h"
typedef enum { HTTP_METHOD_GET, HTTP_METHOD_POST } esp_http_client_method_t;
typedef struct {
    const char *url;
    esp_http_client_method_t method;
    esp_err_t (*crt_bundle_attach)(void *conf);
    int timeout_ms;
    int buffer_size;
    int keep_alive_enable;
    void *user_data;
} esp_http_client_config_t;

#include <stdint.h>
typedef struct esp_http_client *esp_http_client_handle_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value);
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len);
int esp_http_client_write(esp_http_client_handle_t client, const char *buffer, int len);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
/* Location: from the last response, "" if absent. Not an IDF API — the T3 shim
   exposes it so nv_ota_t3.c can follow a redirect itself. */
const char *esp_http_client_get_location(esp_http_client_handle_t client);
/* Re-point an (already closed) handle at another URL, for redirect following. */
esp_err_t esp_http_client_set_url(esp_http_client_handle_t client, const char *url);
int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int len);
esp_err_t esp_http_client_close(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);
