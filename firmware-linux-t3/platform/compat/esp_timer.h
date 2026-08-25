/* Linux shim for ESP-IDF's esp_timer, enough for the shared MMKeypad app
 * code (which uses esp_timer_get_time() as a microsecond monotonic clock). */
#pragma once
#include <stdint.h>
#include <time.h>

static inline int64_t esp_timer_get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
