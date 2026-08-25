// On-device WiFi control for the T3 (AP6330). Drives the cross-built
// wpa_supplicant/wpa_cli (nl80211) + udhcpc that ship in the rootfs. The LVGL
// onboarding screen (wifi_setup.c) calls these; everything shells out to the
// proven bring-up sequence (see thirdparty/build-wpa.sh).
#pragma once
#include <stdbool.h>

typedef struct {
  char ssid[64];
  int  signal;    // dBm (higher = stronger)
  bool secured;   // needs a passphrase (WPA/WPA2/WEP)
} wifi_net_t;

// Load the OOB driver (if needed) + start wpa_supplicant (if not running).
// Idempotent; safe to call repeatedly. Returns true if wlan0 + supplicant are up.
bool wifi_ensure_up(void);

// Scan and return up to `max` networks (deduped by SSID, strongest kept, sorted
// by signal). Blocks ~4s. Returns the count, or -1 on error.
int wifi_scan(wifi_net_t *out, int max);

// Associate to `ssid` with `psk` (NULL/"" for open), then DHCP. Blocks up to
// ~20s. Returns true once wlan0 has an IP. Persists the network to the config.
bool wifi_connect(const char *ssid, const char *psk);

// Wait up to `timeout_s` for the supplicant to reach COMPLETED, then pull a DHCP
// lease. Use after wifi_ensure_up() to bring a *previously saved* network fully
// online (association alone gives no IP). Returns true once wlan0 has an IP.
bool wifi_await_dhcp(int timeout_s);

// True if wlan0 currently holds an IPv4; copies it into `ip` (may be NULL).
bool wifi_have_ip(char *ip, int n);

// Currently-associated SSID ("" if none). Points at a static buffer.
const char *wifi_current_ssid(void);
