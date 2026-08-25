/*
 * nuvoxel_device.h — local, open build.
 *
 * Minimal device-side interface for the open build: a hardware-rooted identity
 * and a local OTA-apply. There is no online service — no check-in, licensing,
 * or registration. Identity comes from nv_identity_open.c (ESP, MAC-derived) or
 * nv_identity_t3.c (T3, eFuse); OTA-apply from nv_ota_open.c (esp_https_ota) or
 * nv_ota_t3.c (overlay swap). The on-screen updater (fwupdate.c) drives it.
 */
#ifndef NUVOXEL_DEVICE_H
#define NUVOXEL_DEVICE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NV_HWID_MAX 40    /* hex hardware id + NUL */

typedef enum {
  NV_OK = 0,
  NV_ERR = -1,
  NV_ERR_ARG = -2,
  NV_ERR_IDENTITY = -3,
  NV_ERR_HTTP = -4,
  NV_ERR_PARSE = -5,
  NV_ERR_SIGNATURE = -6,
  NV_ERR_BINDING = -7,
} nv_err_t;

typedef struct {
  char hardware_id[NV_HWID_MAX];
  bool provisioned;
} nv_identity_t;

/* Local identity: a stable id from the device MAC / eFuse. No secret material,
 * no provisioning handshake. Returns NV_OK on success. */
nv_err_t nv_identity_init(nv_identity_t *out);

/* Apply a firmware image from `url` (sha256 optional, hex, may be NULL/empty).
 * Platform-provided: esp_https_ota on ESP (nv_ota_open.c); the app/init overlay
 * swap on the T3 (nv_ota_t3.c). Restarts into the new image on success. */
nv_err_t nv_ota_apply(const char *url, const char *sha256);

#ifdef __cplusplus
}
#endif

#endif /* NUVOXEL_DEVICE_H */
