#pragma once
#include <stdbool.h>

// Bring up the internal EMAC + external RMII PHY (IP101) and block until the
// interface gets an IP (or a timeout elapses). Mirrors wifi_start()'s contract so
// app_main can pick a transport at compile time. No-op return value mirrors
// wifi_is_up(): use eth_is_up() to poll link/IP state afterward.
void eth_start(void);
bool eth_is_up(void);
