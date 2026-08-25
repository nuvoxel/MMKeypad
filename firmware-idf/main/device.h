// Device identity + self-description for the `hello` handshake and settings.
// Open build: identity is a stable local id (device MAC); there is no online
// service, registration, or licensing.
#pragma once
#include "cJSON.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// Compute the device identity (from the device MAC). Call once early in app_main.
void device_init(void);

// Start device services after networking is up. Open build: no-op (there is no
// online service); kept so main.c's start sequence is unchanged.
void device_start(void);

// Trigger a firmware check. Open build: a no-op — updates come from the
// on-screen GitHub-Releases updater (see fwupdate.c) and USB flashing.
void device_ota_check_now(void);

// Add device identity fields to the settings JSON.
void device_status_to_json(cJSON *d);

// Identity accessors (used by net.c's `hello` so the driver can pin the device).
const char *device_hardware_id(void);
const char *device_sku_id(void);

// Which firmware IMAGE this unit takes, which is not always its SKU: several
// SKUs can share one build (every T3 variant runs the same "mmk-t3" image), so
// the release-asset name is keyed off this, not off device_sku_id(). On boards
// where one SKU means one image it simply returns device_sku_id().
const char *device_fw_image_id(void);

// -------- device manifest (self-description) -----------------------------
// A structured descriptor of what this unit *is* and *can do*: model/SKU,
// SoC + hardware identifiers (MAC, chip id), firmware/protocol versions,
// display, connectivity (wifi/ethernet), power source, and capabilities. The
// device emits it in the `hello` handshake (so the C4 driver adapts to the
// device instead of hardcoding per-SKU). Family-aware: the model is resolved
// from SoC + panel so one
// build serves several Control4 glass models (T3-7 / T3-10 / T4 / T5).
void device_manifest_to_json(cJSON *m);

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
