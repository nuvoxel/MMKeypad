// MMKeypad — BLE bring-up SPIKE (see ble_spike.c).
// Only compiled/called under MMK_BLE_SPIKE (ws43 for now).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Init the hosted BT controller (on the C6 over the esp_hosted SDIO VHCI), init
// NimBLE, and start connectable advertising of the device name. Call AFTER the
// network (wifi_start) is up — it reads the WiFi MAC for the advertised name and
// relies on esp_hosted having already connected to the C6 slave.
void ble_spike_start(void);

#ifdef __cplusplus
}
#endif
