/* Linux shim for ESP-IDF's heap_caps allocator. On the ESP32 these route
 * allocations to internal vs PSRAM by capability flags; on Linux it's all one
 * heap, so the caps are ignored and everything is plain malloc/free. */
#pragma once
#include <stdint.h>
#include <stdlib.h>

#define MALLOC_CAP_8BIT     (1 << 0)
#define MALLOC_CAP_32BIT    (1 << 1)
#define MALLOC_CAP_DMA      (1 << 2)
#define MALLOC_CAP_SPIRAM   (1 << 3)
#define MALLOC_CAP_INTERNAL (1 << 4)
#define MALLOC_CAP_DEFAULT  (1 << 5)

static inline void *heap_caps_malloc(size_t size, uint32_t caps) {
    (void)caps;
    return malloc(size);
}
static inline void *heap_caps_calloc(size_t n, size_t size, uint32_t caps) {
    (void)caps;
    return calloc(n, size);
}
static inline void *heap_caps_realloc(void *p, size_t size, uint32_t caps) {
    (void)caps;
    return realloc(p, size);
}
static inline void heap_caps_free(void *p) { free(p); }
