/*
 * nv_open.c — open-build entitlement (shared by the ESP and T3 builds).
 *
 * The open firmware is not license-gated: nv_entitlement_verify always returns
 * a perpetual, all-features grant, regardless of the token. This replaces the
 * closed component's signed, hardware-bound offline verifier. See
 * nuvoxel_device.h for why.
 *
 * Identity (nv_identity_init) and OTA (nv_ota_apply) are platform-specific and
 * live in nv_identity_open.c / nv_ota_open.c (ESP) and nv_identity_t3.c /
 * nv_ota_t3.c (T3).
 */
#include "nuvoxel_device.h"
#include <stdio.h>
#include <string.h>

/* Every feature the firmware knows how to gate. device_has_feature() also
 * short-circuits to true in the open build, so this is belt-and-suspenders for
 * any path that reads the entitlement's feature list directly. */
#define NV_OPEN_FEATURES "intercom,announce,keypad,nowplaying"

nv_err_t nv_entitlement_verify(const char *token, const nv_identity_t *id,
                               const char *expected_sku, nv_entitlement_t *out) {
  (void)token;
  (void)id;
  if (!out) return NV_ERR_ARG;
  out->valid = true;
  snprintf(out->sku, sizeof(out->sku), "%s", expected_sku ? expected_sku : "");
  snprintf(out->tier, sizeof(out->tier), "%s", "open");
  snprintf(out->features, sizeof(out->features), "%s", NV_OPEN_FEATURES);
  out->trial = false;
  out->exp = 0; /* perpetual */
  return NV_OK;
}
