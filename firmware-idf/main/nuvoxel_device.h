/*
 * nuvoxel_device.h — local, open build.
 *
 * The full product links a closed `nuvoxel_device` component that provides a
 * hardware-rooted identity, cloud enrollment / check-in / OTA, and signed
 * offline entitlement (licensing). The open build in this repository does NOT
 * talk to any online service and is not license-gated, so this header replaces
 * that component with a minimal local implementation (see nv_open.c):
 *
 *   - identity      : a stable id derived from the device MAC / eFuse.
 *   - entitlement   : always valid, all features — no license, no server.
 *   - check-in      : removed. The device never phones home.
 *   - OTA           : local (nv_ota_apply) + the on-screen updater; no cloud
 *                     manifest. See device.c / the Phase-2 updater.
 *
 * The type/prototype surface is kept so device.c / device_t3.c compile with
 * only their online-service call sites removed.
 */
#ifndef NUVOXEL_DEVICE_H
#define NUVOXEL_DEVICE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NV_HWID_MAX 40    /* hex hardware id + NUL */
#define NV_SECRET_MAX 80  /* hex device secret + NUL */

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
  char device_secret[NV_SECRET_MAX];
  bool provisioned;
} nv_identity_t;

/* Local identity: a stable id from the device MAC / eFuse. No secret material,
 * no provisioning handshake. Returns NV_OK on success. */
nv_err_t nv_identity_init(nv_identity_t *out);

typedef struct {
  bool valid;
  char sku[40];
  char tier[24];
  char features[256]; /* comma-joined */
  bool trial;
  long exp;           /* 0 = perpetual */
} nv_entitlement_t;

/* Open build: always returns a perpetual, all-features grant. The token is
 * ignored. No signature check, no server. */
nv_err_t nv_entitlement_verify(const char *token, const nv_identity_t *id,
                               const char *expected_sku, nv_entitlement_t *out);

/* Apply a firmware image from `url` (sha256 optional, hex, may be NULL/empty).
 * Platform-provided: esp_https_ota on ESP (nv_ota_open.c); the app/init overlay
 * swap on the T3 (nv_ota_t3.c). Restarts into the new image on success. */
nv_err_t nv_ota_apply(const char *url, const char *sha256);

#ifdef __cplusplus
}
#endif

#endif /* NUVOXEL_DEVICE_H */
