// On-screen WiFi onboarding for the T3 (big touchscreen — the user picks a
// network from a live scan and types the passphrase on an on-screen keyboard).
// Full-screen LVGL overlay; drives wifi_linux.c on worker threads so the scan
// (~4s) and the connect (~20s) never block the render loop. Closes itself once
// wlan0 has an IP, revealing the keypad UI underneath.
#pragma once

// Show the picker IFF the device is offline (no IPv4 on eth0/wlan0). If a WiFi
// network was saved previously, tries it silently first and only shows the
// picker if that fails. Safe to call once at startup after ui_begin().
void wifi_setup_maybe_show(void);

// Force the picker up regardless of connectivity (e.g. a "WiFi" settings item).
void wifi_setup_show(void);
