#pragma once
#include "esp_log.h"
/* Minimal ESP_*_ON_* checks used by esp_jpeg's jpeg_decoder.c. */
#define ESP_RETURN_ON_FALSE(a, err_code, tag, ...) \
    do { if (!(a)) { ESP_LOGE(tag, ##__VA_ARGS__); return (err_code); } } while (0)
#define ESP_GOTO_ON_FALSE(a, err_code, goto_tag, tag, ...) \
    do { if (!(a)) { ret = (err_code); ESP_LOGE(tag, ##__VA_ARGS__); goto goto_tag; } } while (0)
#define ESP_RETURN_ON_ERROR(x, tag, ...) \
    do { esp_err_t e_ = (x); if (e_ != ESP_OK) { ESP_LOGE(tag, ##__VA_ARGS__); return e_; } } while (0)
#define ESP_GOTO_ON_ERROR(x, goto_tag, tag, ...) \
    do { ret = (x); if (ret != ESP_OK) { ESP_LOGE(tag, ##__VA_ARGS__); goto goto_tag; } } while (0)
