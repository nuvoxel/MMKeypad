// MMKeypad — BLE Wi-Fi provisioning (ESP wifi_provisioning + NimBLE over the C6).
//
// The phone hands the device Wi-Fi credentials over BLE using Espressif's
// wifi_provisioning manager (BLE scheme, PoP security-1). The BT controller
// lives on the onboard ESP32-C6 and is reached over the esp_hosted VHCI (same
// bring-up as ble_spike.c). Received creds persist to NVS and the STA connects;
// the connection then surfaces the same way as the softAP-portal / saved-cred
// paths (IP_EVENT_STA_GOT_IP → wifi.c sets BIT_CONNECTED).
//
// Gated by MMK_HAS_BLE_PROV (defined only in the ws43 board.h section); the
// CMakeLists only compiles this file for ws43. Coexists with the softAP captive
// portal — whichever path connects first wins.

#pragma once
#include "board.h"

#ifdef MMK_HAS_BLE_PROV

// Start BLE provisioning: bring up the C6 BT controller, init the prov manager
// with the NimBLE BLE scheme, and start advertising `service_name` as the BLE
// device name secured by `pop` (proof-of-possession, security-1). Non-blocking:
// the manager runs on its own tasks; creds arrive via the event loop. Safe to
// call after wifi_start() has brought up the esp_hosted transport (esp_wifi up).
void prov_start(const char *service_name, const char *pop);

// Stop + de-init the provisioning manager (idempotent). Called by wifi.c once a
// connection is established (via either the BLE or the portal path). The manager
// also auto-stops itself on a successful provision (WIFI_PROV_END → deinit).
void prov_stop(void);

#endif // MMK_HAS_BLE_PROV
