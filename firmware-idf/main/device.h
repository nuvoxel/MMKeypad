// Device registration + OTA status via the shared nuvoxel_device client.
// Computes the eFuse-HMAC identity at boot and, when the device has an IP,
// best-effort checks in with the platform to learn its registration and OTA
// status (surfaced in the settings page).
#pragma once
#include "cJSON.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// Compute the device identity (eFuse-HMAC). Call once early in app_main.
void device_init(void);

// Spawn the best-effort cloud check-in task (safe to call before WiFi is up;
// it waits for an IP). Call after networking/services are started.
void device_start(void);

// Manual firmware check + force-update from the cloud (driver "Check & Update
// Firmware"). One-shot; no-op if cloud check-in is disabled.
void device_ota_check_now(void);

// Add device / registration / OTA / license fields to the settings JSON.
void device_status_to_json(cJSON *d);

// Apply an out-of-band license token (settings paste / QR / C4 driver). Verifies
// offline against the baked-in key and persists to NVS iff valid. Returns 0 on success.
int device_apply_license(const char *token);

// Identity accessors (used by net.c's `hello` so the C4 driver can fetch a
// license / claim on the device's behalf when the device has no WAN).
const char *device_hardware_id(void);
const char *device_secret_hex(void);
const char *device_sku_id(void);

// One-line status for the on-screen settings card.
const char *device_reg_text(void);   // "Registered" | "Not registered" | "Checking…"
const char *device_pair_code(void);  // pairing code when unregistered, else ""
bool device_is_licensed(void);
const char *device_tier_text(void);  // tier name when licensed, else ""

// True iff the current license grants this named feature (e.g. "intercom",
// "walkup"). Used to gate Pro capabilities — Base keypads return false even when
// the hardware supports it. Always false when unlicensed.
bool device_has_feature(const char *name);

// Human-readable license label for the settings page:
// "Trial (Pro) · 28 days left" | "Pro" | "Base" | "Unlicensed".
const char *device_license_label(void);
bool device_is_trial(void);   // current license is a trial grant

// -------- device manifest (self-description) -----------------------------
// A structured descriptor of what this unit *is* and *can do*: model/SKU,
// SoC + hardware identifiers (MAC, chip id), firmware/protocol versions,
// display, connectivity (wifi/ethernet), power source, and capabilities. The
// device emits it in the `hello` handshake (so the C4 driver adapts to the
// device instead of hardcoding per-SKU) and on platform check-in (stored on
// the station). Family-aware: the model is resolved from SoC + panel so one
// build serves several Control4 glass models (T3-7 / T3-10 / T4 / T5).
void device_manifest_to_json(cJSON *m);

// The full check-in report the device sends to the platform: the static manifest
// plus live status (room, IP, driver connection, orientation, brightness). Stored
// on the station so the fleet UI shows what/where each keypad is.
void device_report_to_json(cJSON *r);

// Individual manifest fields, also surfaced on the on-screen settings page.
const char *device_model_name(void);    // friendly model, e.g. "Control4 T3-7"
const char *device_mac(void);           // primary NIC MAC, "aa:bb:cc:dd:ee:ff"
const char *device_link_type(void);     // active link: "ethernet" | "wifi"
const char *device_power_source(void);  // "wall" | "poe" | "battery"

// The connected C4 driver's own version, relayed driver→device for display in
// settings ("" until the driver reports it). Set by net.c on handshake.
void device_set_driver_version(const char *ver);
const char *device_driver_version(void);

// Short uppercase board tag derived from the SKU: "mmk-t3-10" -> "T3-10",
// "mmk-s3" -> "S3". Falls back to "Keypad" when the SKU is not in the mmk-*
// form. ONE definition so every surface that names the device -- the SDDP Model
// and Host, the setup AP SSID, the BLE advertising name, the claim screen --
// derives it identically instead of each hardcoding a product string.
static inline void device_variant_tag(char *out, size_t n)
{
    const char *sku = device_sku_id();
    size_t j = 0;
    if (sku && !strncmp(sku, "mmk-", 4) && sku[4]) {
        for (const char *c = sku + 4; *c && j + 1 < n; c++, j++)
            out[j] = (*c >= 'a' && *c <= 'z') ? (char)(*c - 'a' + 'A') : *c;
    }
    if (j == 0) { snprintf(out, n, "Keypad"); return; }
    out[j] = '\0';
}
