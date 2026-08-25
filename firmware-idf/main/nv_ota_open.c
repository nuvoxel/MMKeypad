/*
 * nv_ota_open.c — open-build OTA apply for ESP-IDF.
 *
 * Downloads a firmware image over HTTPS and writes it to the inactive OTA
 * partition via esp_https_ota, then restarts into it. The image URL comes from
 * the on-screen updater (GitHub Releases); there is no cloud manifest.
 *
 * sha256 (hex, optional) is logged for now; esp_https_ota already validates the
 * image header and the app descriptor, and the transport is TLS-authenticated
 * against the bundled CA roots. The T3 build uses nv_ota_t3.c instead.
 */
#include "nuvoxel_device.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "nv_ota";

nv_err_t nv_ota_apply(const char *url, const char *sha256) {
  if (!url || !url[0]) return NV_ERR_ARG;
  if (sha256 && sha256[0]) ESP_LOGI(TAG, "expected sha256 %s", sha256);

  esp_http_client_config_t http = {
      .url = url,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
      .timeout_ms = 30000,
      // GitHub does not serve a release asset from the URL you ask for: it
      // answers 302 with a SIGNED redirect to release-assets.githubusercontent.com
      // that runs to ~950 characters (JWT + SAS query). ESP-IDF's HTTP client
      // defaults both buffers to DEFAULT_HTTP_BUF_SIZE (512), so the Location
      // header does not fit in the RX buffer and the redirected request line does
      // not fit in the TX buffer. The download then fails after the UI has already
      // said "downloading", which is what made this look like a server problem.
      .buffer_size = 4096,
      .buffer_size_tx = 4096,
  };
  esp_https_ota_config_t ota = { .http_config = &http };

  ESP_LOGI(TAG, "OTA <- %s", url);
  esp_err_t err = esp_https_ota(&ota);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
    return NV_ERR_HTTP;
  }
  ESP_LOGI(TAG, "OTA image written; restarting into it");
  esp_restart(); /* does not return */
  return NV_OK;
}
