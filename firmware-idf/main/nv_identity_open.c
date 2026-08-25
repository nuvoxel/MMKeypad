/*
 * nv_identity_open.c — open-build device identity (ESP-IDF only).
 *
 * The closed component roots identity in an eFuse HMAC secret. The open build
 * needs no secret (there is no server to authenticate to), so identity is just
 * a stable, human-recognizable id derived from the primary MAC:
 *
 *     hardware_id = "mmk-aabbccddeeff"
 *
 * That id is what the device reports over SDDP and to the Control4 driver, so
 * it stays stable across reboots. The T3 build uses nv_identity_t3.c instead.
 */
#include "nuvoxel_device.h"
#include "esp_mac.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

nv_err_t nv_identity_init(nv_identity_t *out) {
  if (!out) return NV_ERR_ARG;
  memset(out, 0, sizeof(*out));

  uint8_t mac[6] = {0};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK &&
      esp_read_mac(mac, ESP_MAC_ETH) != ESP_OK) {
    return NV_ERR_IDENTITY;
  }
  snprintf(out->hardware_id, sizeof(out->hardware_id),
           "mmk-%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  out->device_secret[0] = '\0';
  out->provisioned = true; /* open build: no enrollment step */
  return NV_OK;
}
