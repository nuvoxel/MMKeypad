/*
 * fwupdate.c — list firmware from GitHub Releases and apply a chosen image.
 *
 * Portable across the ESP and T3 builds: uses the esp_http_client interface
 * (native on ESP, shimmed by httpc_linux.c on the T3), cJSON, and FreeRTOS
 * tasks (shimmed on the T3, same as art.c / net.c). The UI polls the state and
 * reads the result array; nothing here runs on the LVGL thread.
 *
 * Asset convention: a release carries one image per SKU named
 *   <sku>-<version>.bin      e.g.  mmk-s3-2026.08.24.001.bin
 * where <sku> is device_sku_id(). Only assets matching THIS device's SKU are
 * offered.
 */
#include "fwupdate.h"
#include "nuvoxel_device.h"   // nv_ota_apply
#include "device.h"           // device_sku_id
#include "config.h"           // fw_version
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "fwupdate";

// The releases API for this project. Override at build time if you fork.
#ifndef FWUPDATE_RELEASES_URL
#define FWUPDATE_RELEASES_URL \
  "https://api.github.com/repos/nuvoxel/MMKeypad/releases?per_page=30"
#endif

#define FWU_MAX     16          // most versions we list
#define FWU_BODYCAP (128 * 1024) // response cap; releases JSON is well under this

static volatile fwupdate_state_t s_state = FWU_IDLE;
static fwupdate_rel_t s_rel[FWU_MAX];
static int s_count;
static char s_err[64];
static int s_apply_idx;

fwupdate_state_t fwupdate_state(void) { return s_state; }
int fwupdate_count(void) { return s_state == FWU_READY ? s_count : 0; }
const fwupdate_rel_t *fwupdate_get(int i) {
  return (i >= 0 && i < s_count) ? &s_rel[i] : NULL;
}
const char *fwupdate_error(void) { return s_err; }

// Running version with the trailing type tag ("FW") stripped, for the "current"
// flag: fw_version() is "2026.08.24.001FW", assets carry "2026.08.24.001".
static void running_core(char *out, size_t cap) {
  snprintf(out, cap, "%s", fw_version());
  size_t n = strlen(out);
  while (n > 0 && (out[n - 1] < '0' || out[n - 1] > '9')) out[--n] = '\0';
}

// Read the whole response body into buf (NUL-terminated). Returns length, or -1.
static int http_get(const char *url, char *buf, int cap) {
  esp_http_client_config_t cfg = {
      .url = url,
      .crt_bundle_attach = esp_crt_bundle_attach,
  };
  esp_http_client_handle_t cli = esp_http_client_init(&cfg);
  if (!cli) return -1;
  esp_http_client_set_header(cli, "User-Agent", "mmkeypad");
  esp_http_client_set_header(cli, "Accept", "application/vnd.github+json");

  int total = -1;
  if (esp_http_client_open(cli, 0) == ESP_OK) {
    esp_http_client_fetch_headers(cli);
    total = 0;
    int r;
    while (total < cap - 1 &&
           (r = esp_http_client_read(cli, buf + total, cap - 1 - total)) > 0) {
      total += r;
    }
    buf[total] = '\0';
    esp_http_client_close(cli);
  }
  esp_http_client_cleanup(cli);
  return total;
}

// Parse the releases JSON, filling s_rel with the assets that match our SKU.
static void parse_releases(const char *body) {
  char sku[48];
  snprintf(sku, sizeof(sku), "%s-", device_sku_id());  // "mmk-s3-"
  size_t skulen = strlen(sku);
  char core[48];
  running_core(core, sizeof(core));

  cJSON *root = cJSON_Parse(body);
  s_count = 0;
  if (!cJSON_IsArray(root)) {
    cJSON_Delete(root);
    snprintf(s_err, sizeof(s_err), "bad response from GitHub");
    s_state = FWU_ERROR;
    return;
  }

  cJSON *rel;
  cJSON_ArrayForEach(rel, root) {
    if (s_count >= FWU_MAX) break;
    if (cJSON_IsTrue(cJSON_GetObjectItem(rel, "draft"))) continue;
    cJSON *tag = cJSON_GetObjectItem(rel, "tag_name");
    cJSON *assets = cJSON_GetObjectItem(rel, "assets");
    if (!cJSON_IsString(tag) || !cJSON_IsArray(assets)) continue;

    cJSON *asset;
    cJSON_ArrayForEach(asset, assets) {
      cJSON *name = cJSON_GetObjectItem(asset, "name");
      cJSON *url = cJSON_GetObjectItem(asset, "browser_download_url");
      if (!cJSON_IsString(name) || !cJSON_IsString(url)) continue;
      const char *nm = name->valuestring;
      size_t nl = strlen(nm);
      if (strncmp(nm, sku, skulen) != 0) continue;       // wrong SKU
      if (nl < 4 || strcmp(nm + nl - 4, ".bin") != 0) continue;

      fwupdate_rel_t *e = &s_rel[s_count++];
      snprintf(e->version, sizeof(e->version), "%s", tag->valuestring);
      snprintf(e->url, sizeof(e->url), "%s", url->valuestring);
      cJSON *sz = cJSON_GetObjectItem(asset, "size");
      e->size = cJSON_IsNumber(sz) ? (long)sz->valuedouble : 0;
      e->current = (core[0] && strstr(nm, core) != NULL);
      break;  // one asset per release for this SKU
    }
  }
  cJSON_Delete(root);

  if (s_count == 0) {
    snprintf(s_err, sizeof(s_err), "no builds published for %s", device_sku_id());
    s_state = FWU_ERROR;
  } else {
    s_state = FWU_READY;
  }
}

static void fetch_task(void *arg) {
  (void)arg;
  char *body = malloc(FWU_BODYCAP);
  if (!body) {
    snprintf(s_err, sizeof(s_err), "out of memory");
    s_state = FWU_ERROR;
    vTaskDelete(NULL);
    return;
  }
  int n = http_get(FWUPDATE_RELEASES_URL, body, FWU_BODYCAP);
  if (n <= 0) {
    snprintf(s_err, sizeof(s_err), "couldn't reach GitHub");
    s_state = FWU_ERROR;
  } else {
    ESP_LOGI(TAG, "releases: %d bytes", n);
    parse_releases(body);
  }
  free(body);
  vTaskDelete(NULL);
}

void fwupdate_start_fetch(void) {
  if (s_state == FWU_FETCHING || s_state == FWU_APPLYING) return;
  s_count = 0;
  s_err[0] = '\0';
  s_state = FWU_FETCHING;
  xTaskCreate(fetch_task, "fwu_fetch", 8192, NULL, 4, NULL);
}

static void apply_task(void *arg) {
  (void)arg;
  const fwupdate_rel_t *e = fwupdate_get(s_apply_idx);
  if (!e) {
    snprintf(s_err, sizeof(s_err), "no such version");
    s_state = FWU_ERROR;
    vTaskDelete(NULL);
    return;
  }
  ESP_LOGI(TAG, "applying %s <- %s", e->version, e->url);
  // nv_ota_apply reboots into the new image on success and does not return.
  nv_ota_apply(e->url, NULL);
  snprintf(s_err, sizeof(s_err), "update failed");
  s_state = FWU_ERROR;
  vTaskDelete(NULL);
}

void fwupdate_apply(int i) {
  if (s_state != FWU_READY) return;
  if (i < 0 || i >= s_count) return;
  s_apply_idx = i;
  s_state = FWU_APPLYING;
  xTaskCreate(apply_task, "fwu_apply", 8192, NULL, 4, NULL);
}
