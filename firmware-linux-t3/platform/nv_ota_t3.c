/*
 * nv_ota_apply for the T3 (RK3188 / Linux) — persistent app-overlay OTA.
 *
 * The rootfs is an initramfs (RAM), rebuilt from boot.img every boot, so
 * overwriting files there would not survive a reboot. Instead the OTA artifact
 * (a tar bundle published for the mmk-t3 SKU) installs BOTH updatable overlays to
 * the persistent NAND /data partition:
 *
 *     /data/mmkeypad         ← the app overlay (init prefers it over factory)
 *     /data/init.overlay     ← the init overlay (bootstrap execs it; see init.c)
 *     *.prev                 ← previous copies, kept for rollback
 *
 * The bootstrap init (boot.img) runs /data/init.overlay when present+healthy and
 * rolls it back on crash-loop; that worker runs /data/mmkeypad and rolls IT back
 * on crash-loop. So OTA updates the app + init together with zero brick risk and
 * never rewrites the boot partition — only the stock kernel stays fixed. OTA
 * here is: download → sha256-verify → extract → swap the overlays → reboot (if
 * init changed) or exit (app-only) so the new build runs.
 *
 * HTTP + TLS reuse the platform's esp_http_client shim (httpc_linux.c, mbedtls);
 * verification reuses mbedtls sha256. This file is target-native (compiled
 * without ESP_PLATFORM) and provides nv_ota_apply in place of the component's.
 */
#include "nuvoxel_device.h"

#include "esp_http_client.h"
#include "mbedtls/sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define OTA_DIR      "/data/nvx"
#define OTA_TMP      OTA_DIR "/ota.download"
#define APP_OVERLAY  "/data/mmkeypad"
#define APP_PREV     "/data/mmkeypad.prev"
#define T3_BUNDLE_MAX 1   /* highest t3-bundle formatVersion this build accepts */

static void ota_hex(const unsigned char *d, int n, char *o) {
  static const char *H = "0123456789abcdef";
  for (int i = 0; i < n; i++) {
    o[i * 2] = H[d[i] >> 4];
    o[i * 2 + 1] = H[d[i] & 0xf];
  }
  o[n * 2] = '\0';
}

nv_err_t nv_ota_apply(const char *url, const char *sha256) {
  if (!url) return NV_ERR_ARG;

  mkdir(OTA_DIR, 0700);

  esp_http_client_config_t http = {
      .url = url,
      .timeout_ms = 30000,
  };
  esp_http_client_handle_t c = esp_http_client_init(&http);
  if (!c) return NV_ERR;

  nv_err_t rc = NV_ERR;
  FILE *out = NULL;
  mbedtls_sha256_context sh;
  mbedtls_sha256_init(&sh);

  if (esp_http_client_open(c, 0) != ESP_OK) { rc = NV_ERR_HTTP; goto done; }
  esp_http_client_fetch_headers(c);
  if (esp_http_client_get_status_code(c) != 200) { rc = NV_ERR_HTTP; goto done; }

  out = fopen(OTA_TMP, "wb");
  if (!out) goto done;
  mbedtls_sha256_starts(&sh, 0);

  {
    char buf[4096];
    int r;
    while ((r = esp_http_client_read(c, buf, sizeof(buf))) > 0) {
      mbedtls_sha256_update(&sh, (const unsigned char *)buf, r);
      if (fwrite(buf, 1, r, out) != (size_t)r) { rc = NV_ERR; goto done; }
    }
    if (r < 0) { rc = NV_ERR_HTTP; goto done; }
  }
  fclose(out);
  out = NULL;

  {
    unsigned char digest[32];
    char hex[65];
    mbedtls_sha256_finish(&sh, digest);
    ota_hex(digest, 32, hex);
    if (sha256 && sha256[0] && strcasecmp(hex, sha256) != 0) {
      fprintf(stderr, "ota: sha256 mismatch (got %s want %s)\n", hex, sha256);
      unlink(OTA_TMP);
      rc = NV_ERR_SIGNATURE;
      goto done;
    }
  }

  // The T3 OTA artifact is a tar BUNDLE carrying both overlays:
  //   mmkeypad     -> /data/mmkeypad     (the app; init respawns into it)
  //   init.overlay -> /data/init.overlay (the OTA'able init; bootstrap execs it)
  // Extract, then install each to its persistent /data overlay (keeping .prev for
  // rollback). If the init overlay changed we REBOOT so the bootstrap runs the
  // new init under its trial/quarantine protection; otherwise just exit for PID 1
  // to respawn the app. (init.overlay may be absent -> app-only update.)
  system("rm -rf " OTA_DIR "/x && /bin/busybox mkdir -p " OTA_DIR "/x");
  {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "/bin/busybox tar -xf %s -C %s/x 2>/dev/null", OTA_TMP, OTA_DIR);
    if (system(cmd) != 0) { rc = NV_ERR; goto done; }
  }
  unlink(OTA_TMP);

  // Gate on the bundle's self-declared format version (meta.json) before we trust
  // the layout, so an older device refuses a newer bundle format rather than
  // mis-installing it. Crude parse (no JSON lib in init/ota land): look for the
  // "t3-bundle" tag and "version": N.
  {
    char meta[512] = {0};
    FILE *mf = fopen(OTA_DIR "/x/meta.json", "r");
    if (mf) { size_t n = fread(meta, 1, sizeof(meta) - 1, mf); meta[n] = 0; fclose(mf); }
    int bver = 0;
    char *v = strstr(meta, "\"version\"");
    if (v && (v = strchr(v, ':'))) bver = atoi(v + 1);
    if (!strstr(meta, "t3-bundle") || bver < 1 || bver > T3_BUNDLE_MAX) {
      fprintf(stderr, "ota: unsupported t3-bundle format (%s) -> refusing\n",
              meta[0] ? meta : "no meta.json");
      system("rm -rf " OTA_DIR "/x");
      rc = NV_ERR;
      goto done;
    }
  }

  {
    const char *newapp = OTA_DIR "/x/mmkeypad";
    if (access(newapp, F_OK) == 0) {
      chmod(newapp, 0755);
      rename(APP_OVERLAY, APP_PREV);          // rollback target (crash-loop revert)
      if (rename(newapp, APP_OVERLAY) != 0) {
        rename(APP_PREV, APP_OVERLAY);
        goto done;
      }
    }
  }

  int init_changed = 0;
  {
    const char *newinit = OTA_DIR "/x/init.overlay";
    if (access(newinit, F_OK) == 0) {
      chmod(newinit, 0755);
      char cmd[256];
      snprintf(cmd, sizeof(cmd), "/bin/busybox cmp -s %s /data/init.overlay 2>/dev/null", newinit);
      if (system(cmd) != 0) { /* differs (or none installed) -> update it */
        rename("/data/init.overlay", "/data/init.overlay.prev");
        if (rename(newinit, "/data/init.overlay") == 0) {
          unlink("/data/init.overlay.bad");   // clear any prior quarantine
          FILE *tf = fopen(OTA_DIR "/init.trial", "w"); /* fresh trials */
          if (tf) { fputs("0\n", tf); fclose(tf); }
          init_changed = 1;
        } else {
          rename("/data/init.overlay.prev", "/data/init.overlay");
        }
      }
    }
  }
  system("rm -rf " OTA_DIR "/x");
  sync();

  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  mbedtls_sha256_free(&sh);
  if (init_changed) {
    // The new init only takes effect on the next boot (the bootstrap execs it),
    // so reboot; the bootstrap rolls it back automatically if it crash-loops.
    fprintf(stderr, "ota: app+init updated; rebooting to run new init\n");
    system("/bin/busybox reboot -f");
  } else {
    fprintf(stderr, "ota: app updated; exiting for init to respawn\n");
  }
  _exit(0);

done:
  if (out) fclose(out);
  mbedtls_sha256_free(&sh);
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  return rc;
}
