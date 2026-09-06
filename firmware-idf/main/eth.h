#pragma once
#include <stdbool.h>

// Bring up the internal EMAC + external RMII PHY (IP101) and block until the
// interface gets an IP (or a timeout elapses). Mirrors wifi_start()'s contract so
// app_main can pick a transport at compile time. No-op return value mirrors
// wifi_is_up(): use eth_is_up() to poll link/IP state afterward.
void eth_start(void);
bool eth_is_up(void);

// Give up on wired for this boot: stop the driver and put the PHY back in reset.
// Called when we fall back to WiFi, to hold the "never two interfaces at once"
// invariant that app_main documents -- eth_start() returns after its DHCP timeout
// but leaves the driver running, so without this a lease arriving late lands a
// second address on the device after WiFi is already up. Idempotent.
void eth_stand_down(void);
