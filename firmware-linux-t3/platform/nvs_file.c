/* Flat-file NVS backend for the T3 Linux target. One namespace, stored as
 * "key=value\n" lines at /data/mmkeypad.conf (uint values only, which is all
 * config.c uses). Loaded into a small in-memory table on open; committed back
 * on nvs_commit. */
#include "nvs.h"

#include <stdio.h>
#include <string.h>

#define NVS_PATH "/data/mmkeypad.conf"
#define MAX_KEYS 32

struct nvs_store {
    char keys[MAX_KEYS][16];
    uint32_t vals[MAX_KEYS];
    int n;
    int dirty;
};

static struct nvs_store g_store; /* single namespace is enough */

static int find(struct nvs_store *s, const char *key) {
    for (int i = 0; i < s->n; i++)
        if (strncmp(s->keys[i], key, sizeof(s->keys[i])) == 0)
            return i;
    return -1;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out) {
    (void)ns; (void)mode;
    struct nvs_store *s = &g_store;
    s->n = 0;
    s->dirty = 0;
    FILE *f = fopen(NVS_PATH, "r");
    if (f) {
        char line[64];
        while (fgets(line, sizeof(line), f) && s->n < MAX_KEYS) {
            char k[16];
            unsigned v;
            if (sscanf(line, "%15[^=]=%u", k, &v) == 2) {
                strncpy(s->keys[s->n], k, sizeof(s->keys[s->n]) - 1);
                s->vals[s->n] = v;
                s->n++;
            }
        }
        fclose(f);
    }
    *out = s;
    return ESP_OK;
}

void nvs_close(nvs_handle_t h) { (void)h; }

esp_err_t nvs_commit(nvs_handle_t h) {
    if (!h || !h->dirty)
        return ESP_OK;
    FILE *f = fopen(NVS_PATH, "w");
    if (!f)
        return ESP_FAIL;
    for (int i = 0; i < h->n; i++)
        fprintf(f, "%s=%u\n", h->keys[i], h->vals[i]);
    fclose(f);
    h->dirty = 0;
    return ESP_OK;
}

static esp_err_t get_u32(nvs_handle_t h, const char *key, uint32_t *out) {
    if (!h) return ESP_FAIL;
    int i = find(h, key);
    if (i < 0) return ESP_ERR_NOT_FOUND;
    *out = h->vals[i];
    return ESP_OK;
}

static esp_err_t set_u32(nvs_handle_t h, const char *key, uint32_t val) {
    if (!h) return ESP_FAIL;
    int i = find(h, key);
    if (i < 0) {
        if (h->n >= MAX_KEYS) return ESP_ERR_NO_MEM;
        i = h->n++;
        strncpy(h->keys[i], key, sizeof(h->keys[i]) - 1);
        h->keys[i][sizeof(h->keys[i]) - 1] = 0;
    }
    h->vals[i] = val;
    h->dirty = 1;
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out) {
    uint32_t v;
    esp_err_t e = get_u32(h, key, &v);
    if (e == ESP_OK) *out = (uint8_t)v;
    return e;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t val) {
    return set_u32(h, key, val);
}
esp_err_t nvs_get_u16(nvs_handle_t h, const char *key, uint16_t *out) {
    uint32_t v;
    esp_err_t e = get_u32(h, key, &v);
    if (e == ESP_OK) *out = (uint16_t)v;
    return e;
}
esp_err_t nvs_set_u16(nvs_handle_t h, const char *key, uint16_t val) {
    return set_u32(h, key, val);
}
