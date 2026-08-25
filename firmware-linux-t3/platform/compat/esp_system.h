#pragma once
/* esp_jpeg's jpeg_decoder.c includes this and expects bool/assert/heap_caps
 * transitively (ESP-IDF pulls them via the component chain). Provide them. */
#include <assert.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_heap_caps.h"
void esp_restart(void) __attribute__((noreturn));
