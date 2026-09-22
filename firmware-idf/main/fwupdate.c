/*
 * fwupdate.c — list firmware from GitHub Releases and apply a chosen image.
 *
 * Portable across the ESP and T3 builds: uses the esp_http_client interface
 * (native on ESP, shimmed by httpc_linux.c on the T3), cJSON, and FreeRTOS
 * tasks (shimmed on the T3, same as art.c / net.c). The UI polls the state and
 * reads the result array; nothing here runs on the LVGL thread.
 *
 * Asset convention: a release carries one image per FIRMWARE IMAGE named
 *   <image-id>-<version><ext>   e.g.  mmk-s3-2026.08.24.001.bin
 *                                     mmk-t3-2026.08.25.004.tar
 * where <image-id> is device_fw_image_id() -- the image this unit takes, which
 * is not always its SKU (every T3 variant shares the one "mmk-t3" build). Only
 * assets matching THIS device's image are offered.
 *
 * <ext> is per platform because the artifacts genuinely differ: the ESP boards
 * take a raw app image, the T3 takes a `t3-bundle` tar (app + init overlay) that
 * nv_ota_apply unpacks. Matching on the wrong extension is how the T3 ended up
 * unable to see any release at all.
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

// Where the release list comes from. Override at build time if you fork.
//
// PRIMARY is the releases Atom feed on github.com, not the REST API. The API is
// what panels kept failing on: unauthenticated it allows 60 requests/hour PER
// EGRESS IP -- shared by every panel, every laptop and anything else on the LAN
// that talks to api.github.com -- and answers a 403 when that runs out, plus its
// per-release JSON is ~12KB (full asset metadata for every SKU) so the list is
// ~200KB and ever-growing. The feed is served like any web page: no rate limit,
// ~10KB for the ten newest releases, one <title>vTAG</title> per release. Asset
// URLs are then built by convention (releases/download/<tag>/<image>-<ver><ext>),
// which is the same convention tools/verify-release.sh enforces at publish time.
//
// The API stays as the FALLBACK (feed unreachable or reshaped): it also carries
// asset sizes, which the feed path leaves at 0 ("unknown").
#ifndef FWUPDATE_FEED_URL
#define FWUPDATE_FEED_URL "https://github.com/nuvoxel/MMKeypad/releases.atom"
#endif
#ifndef FWUPDATE_DOWNLOAD_BASE
#define FWUPDATE_DOWNLOAD_BASE "https://github.com/nuvoxel/MMKeypad/releases/download"
#endif
// per_page capped near FWU_MAX (not 30): GitHub's per-release JSON is verbose (full
// asset metadata x5 SKUs), so this project's actual releases list grew from ~30KB
// to 178KB over ten releases published in one day -- comfortably over the old
// per_page=30 request's response size vs. FWU_BODYCAP, silently truncating the JSON
// mid-object and turning into "bad response from GitHub" (cJSON_Parse fails on
// truncated input). The device only ever shows FWU_MAX releases anyway; asking for
// exactly that many keeps the response bounded as the release history keeps growing.
#ifndef FWUPDATE_RELEASES_URL
#define FWUPDATE_RELEASES_URL \
  "https://api.github.com/repos/nuvoxel/MMKeypad/releases?per_page=16"
#endif

// Artifact this platform installs. The T3's updater takes a t3-bundle tar (app +
// init overlay); every ESP board takes a raw app image.
#ifdef MMK_BOARD_T3
#define FWU_ASSET_EXT ".tar"
#else
#define FWU_ASSET_EXT ".bin"
#endif
#define FWU_EXT_LEN (sizeof(FWU_ASSET_EXT) - 1)

#define FWU_MAX     16          // most versions we list
#define FWU_BODYCAP (256 * 1024) // response cap -- see FWUPDATE_RELEASES_URL's
                                  // comment; this is margin over today's real size
                                  // (178KB for 10 releases), not a number the
                                  // per_page cap above is expected to reach

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
// *out_status is the HTTP status code (0 if we never got a response at all) --
// GitHub's unauthenticated rate limit (60/hr, shared across the whole LAN's
// egress IP) returns a 403/429 with a JSON *object* body ({"message":...}),
// which used to reach the caller indistinguishable from any other non-array
// response and get reported as the generic "bad response from GitHub".
static int http_get(const char *url, char *buf, int cap, int *out_status) {
  esp_http_client_config_t cfg = {
      .url = url,
      .crt_bundle_attach = esp_crt_bundle_attach,
      // github.com answers with ~5KB of headers (see FWUPDATE_FEED_URL); give the
      // client the same room nv_ota_open.c gives the asset download.
      .buffer_size = 4096,
  };
  esp_http_client_handle_t cli = esp_http_client_init(&cfg);
  if (!cli) return -1;
  esp_http_client_set_header(cli, "User-Agent", "mmkeypad");
  esp_http_client_set_header(cli, "Accept", "application/vnd.github+json");

  int total = -1;
  if (esp_http_client_open(cli, 0) == ESP_OK) {
    esp_http_client_fetch_headers(cli);
    *out_status = esp_http_client_get_status_code(cli);
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

// Parse the releases API JSON, filling s_rel with the assets that match our SKU.
// `status` is the HTTP status http_get() saw, purely to make a non-array body
// (rate limit, GitHub outage) diagnosable instead of a bare "bad response".
// Returns true with s_rel filled (possibly to zero entries -- see s_count), false
// with s_err set when the body was not a usable release list.
static bool parse_releases(const char *body, int status) {
  char sku[48];
  snprintf(sku, sizeof(sku), "%s-", device_fw_image_id());  // "mmk-s3-" / "mmk-t3-"
  size_t skulen = strlen(sku);
  char core[48];
  running_core(core, sizeof(core));

  cJSON *root = cJSON_Parse(body);
  s_count = 0;
  if (!cJSON_IsArray(root)) {
    // A 403/429 body is a JSON OBJECT ({"message":"API rate limit exceeded..."}),
    // valid JSON that just isn't the array we expect -- surface the status (and
    // GitHub's own message, if it parsed) instead of the old undifferentiated
    // "bad response", which this and a GitHub-side outage both looked like.
    cJSON *msg = cJSON_IsObject(root) ? cJSON_GetObjectItem(root, "message") : NULL;
    if (status == 403 || status == 429)
      snprintf(s_err, sizeof(s_err), "GitHub rate-limited us (%d)%s%s", status,
               cJSON_IsString(msg) ? ": " : "", cJSON_IsString(msg) ? msg->valuestring : "");
    else if (status && status != 200)
      snprintf(s_err, sizeof(s_err), "GitHub returned HTTP %d", status);
    else
      snprintf(s_err, sizeof(s_err), "bad response from GitHub");
    ESP_LOGW(TAG, "releases parse failed, status=%d body[0..120]=%.120s", status, body);
    cJSON_Delete(root);
    return false;
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
      if (strncmp(nm, sku, skulen) != 0) continue;       // wrong image
      if (nl < FWU_EXT_LEN ||
          strcmp(nm + nl - FWU_EXT_LEN, FWU_ASSET_EXT) != 0) continue;

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
  return true;
}

// Parse the releases Atom feed: one <entry> per release, its tag in <title>. Assets
// are not listed there, so each entry's URL is built by the publishing convention
// (OTA.md): <base>/<tag>/<image-id>-<version><ext>, e.g.
//   https://github.com/nuvoxel/MMKeypad/releases/download/v2026.09.18.001/mmk-ws43-2026.09.18.001.bin
// A release published without this SKU's asset (verify-release.sh exists to stop
// that) shows in the list and then fails at download with a 404, which nv_ota_apply
// reports as "update failed" -- the same as any other bad download.
static bool parse_feed(const char *body) {
  char core[48];
  running_core(core, sizeof(core));
  s_count = 0;
  const char *p = body;
  bool seen_entry = false;
  while ((p = strstr(p, "<entry>")) != NULL) {
    seen_entry = true;
    const char *end = strstr(p, "</entry>");
    const char *t = strstr(p, "<title>");
    if (!t || (end && t > end)) { p += 7; continue; }
    t += 7;
    const char *te = strstr(t, "</title>");
    if (!te || te - t <= 0 || te - t >= 47) { p += 7; continue; }
    char tag[48];
    memcpy(tag, t, te - t);
    tag[te - t] = '\0';
    // Tag -> version core: strip a leading 'v'. Tags are "v2026.09.18.001".
    const char *ver = (tag[0] == 'v' || tag[0] == 'V') ? tag + 1 : tag;
    if (!ver[0]) { p += 7; continue; }
    if (s_count < FWU_MAX) {
      fwupdate_rel_t *e = &s_rel[s_count++];
      snprintf(e->version, sizeof(e->version), "%s", tag);
      snprintf(e->url, sizeof(e->url), "%s/%s/%s-%s%s", FWUPDATE_DOWNLOAD_BASE, tag,
               device_fw_image_id(), ver, FWU_ASSET_EXT);
      e->size = 0;
      e->current = (core[0] && strcmp(ver, core) == 0);
    }
    p = end ? end : te;
  }
  if (!seen_entry) {
    snprintf(s_err, sizeof(s_err), "bad feed from GitHub");
    ESP_LOGW(TAG, "feed parse failed, body[0..120]=%.120s", body);
    return false;
  }
  return true;
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
  int status = 0;
  bool ok = false;

  // 1. The Atom feed (no rate limit, small). See FWUPDATE_FEED_URL.
  int n = http_get(FWUPDATE_FEED_URL, body, FWU_BODYCAP, &status);
  if (n > 0 && status == 200 && n < FWU_BODYCAP - 1) {
    ESP_LOGI(TAG, "feed: %d bytes", n);
    ok = parse_feed(body);
  } else {
    ESP_LOGW(TAG, "feed fetch failed: n=%d status=%d", n, status);
  }

  // 2. The REST API, if the feed didn't work out.
  if (!ok) {
    status = 0;
    n = http_get(FWUPDATE_RELEASES_URL, body, FWU_BODYCAP, &status);
    if (n == FWU_BODYCAP - 1) {
      // http_get() stopping exactly at cap-1 means the body filled the buffer and got
      // cut off mid-read, not that GitHub's response happened to be exactly this size --
      // the ambiguous truncated-JSON parse failure this used to surface as ("bad
      // response from GitHub") is exactly this, just diagnosed instead of guessed at.
      snprintf(s_err, sizeof(s_err), "release list too large (%d KB) -- FWU_BODYCAP", n / 1024);
    } else if (n <= 0) {
      snprintf(s_err, sizeof(s_err), "couldn't reach GitHub");
    } else {
      ESP_LOGI(TAG, "releases: %d bytes, status=%d", n, status);
      ok = parse_releases(body, status);
    }
  }

  if (!ok) {
    s_state = FWU_ERROR;                       // s_err set by whichever step failed last
  } else if (s_count == 0) {
    snprintf(s_err, sizeof(s_err), "no builds published for %s", device_fw_image_id());
    s_state = FWU_ERROR;
  } else {
    s_state = FWU_READY;
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

// ── Remote trigger (see fwupdate.h) ─────────────────────────────────────────
static char s_want[48];

static void update_now_task(void *arg) {
  (void)arg;
  fwupdate_start_fetch();
  // Bounded wait — fetch_task always lands on READY or ERROR, but never spin here
  // on the assumption that it does.
  for (int i = 0; i < 600 && s_state == FWU_FETCHING; i++) vTaskDelay(pdMS_TO_TICKS(100));
  if (s_state != FWU_READY) {
    ESP_LOGW(TAG, "remote update: fetch failed (%s)", s_err[0] ? s_err : "timed out");
    vTaskDelete(NULL);
    return;
  }
  int pick = -1;
  if (s_want[0]) {
    for (int i = 0; i < s_count; i++)
      if (strstr(s_rel[i].version, s_want)) { pick = i; break; }
    if (pick < 0) ESP_LOGW(TAG, "remote update: no release matching '%s'", s_want);
  } else if (s_count > 0 && !s_rel[0].current) {
    pick = 0;                 // newest-first, and it is not what we are running
  } else {
    ESP_LOGI(TAG, "remote update: already on the newest release");
  }
  if (pick >= 0) {
    ESP_LOGW(TAG, "remote update: applying %s", s_rel[pick].version);
    fwupdate_apply(pick);     // reboots on success
  }
  vTaskDelete(NULL);
}

bool fwupdate_update_now(const char *version) {
  if (s_state == FWU_FETCHING || s_state == FWU_APPLYING) return false;
  s_want[0] = '\0';
  if (version && version[0]) snprintf(s_want, sizeof(s_want), "%s", version);
  xTaskCreate(update_now_task, "fwu_now", 4096, NULL, 4, NULL);
  return true;
}

void fwupdate_apply(int i) {
  if (s_state != FWU_READY) return;
  if (i < 0 || i >= s_count) return;
  s_apply_idx = i;
  s_state = FWU_APPLYING;
  xTaskCreate(apply_task, "fwu_apply", 8192, NULL, 4, NULL);
}
