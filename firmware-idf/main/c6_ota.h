// c6_ota — host-driven OTA of the onboard ESP32-C6 esp_hosted slave.
//
// Reads a slave app image (network_adapter.bin) that was raw-flashed into the
// P4's `model` data partition, and streams it to the C6 over the existing
// esp_hosted transport (SDIO) via the esp_hosted_slave_ota_* RPC. Used once, on
// a bench build, to move the factory C6 slave onto a BT-capable 2.12.x slave so
// esp_hosted_bt_controller_init() (NimBLE) stops timing out on Req_FeatureControl.
//
// Gated by MMK_C6_OTA (defined in board.h, ws43 section only, DEFAULT OFF). The
// call site in app_main is #ifdef MMK_C6_OTA; this file always compiles on ws43.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Run the C6 slave OTA from the `model` partition. Skips (no-op) if the slave
// already reports the image's version. On success, activates the new slave
// firmware and reboots the host to re-sync the transport. Safe to leave the
// call compiled in: it is only reached when MMK_C6_OTA is defined at the site.
void c6_ota_run(void);

#ifdef __cplusplus
}
#endif
