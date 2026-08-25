/*
 * fwupdate.h — on-screen firmware updates from GitHub Releases.
 *
 * The open build has no cloud check-in. Instead the device can list the
 * firmware images published as assets on this project's GitHub Releases and let
 * the user pick one on-screen. It matches assets named "<sku>-<version>.bin"
 * (sku = device_sku_id(), e.g. "mmk-s3") and applies a chosen image via
 * nv_ota_apply(), which reboots into it.
 *
 * All network work runs on a background task; the UI polls fwupdate_state() and
 * reads the result list, so nothing here blocks the LVGL thread. Shared verbatim
 * by the ESP and T3 builds.
 */
#ifndef MMK_FWUPDATE_H
#define MMK_FWUPDATE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  FWU_IDLE = 0,     // nothing fetched yet
  FWU_FETCHING,     // querying GitHub
  FWU_READY,        // fwupdate_count() results available
  FWU_ERROR,        // fetch failed (no net / API / parse)
  FWU_APPLYING,     // downloading + writing a chosen image (reboot imminent)
} fwupdate_state_t;

typedef struct {
  char version[48]; // release tag, e.g. "v2026.08.24.001"
  char url[256];    // asset download URL for this device's SKU
  long size;        // asset size in bytes (0 if unknown)
  bool current;     // true if this matches the running firmware version
} fwupdate_rel_t;

// Kick off a background fetch of the release list (no-op if already fetching or
// applying). State goes FETCHING -> READY|ERROR.
void fwupdate_start_fetch(void);

fwupdate_state_t fwupdate_state(void);

// Valid once state == FWU_READY. Newest first; the running version is flagged
// with .current.
int fwupdate_count(void);
const fwupdate_rel_t *fwupdate_get(int i);

// A short human-readable line for the last error (empty until one occurs).
const char *fwupdate_error(void);

// Download + apply result i (from the last fetch). Runs on a background task and
// reboots into the new image on success; sets FWU_ERROR if it fails.
void fwupdate_apply(int i);

#ifdef __cplusplus
}
#endif

#endif /* MMK_FWUPDATE_H */
