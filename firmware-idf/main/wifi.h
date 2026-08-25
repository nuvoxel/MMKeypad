#pragma once
#include <stdbool.h>

// TEMP bench path: STA-connect using the Kconfig credentials so the net layer is
// reachable for testing. The real first-boot flow (SoftAP captive portal) replaces
// this. No-op if no bench SSID is configured.
void wifi_start(void);
bool wifi_is_up(void);
