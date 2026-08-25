#pragma once
#include <stdint.h>

// Firmware version — single source of truth is the ESP-IDF app descriptor, fed by
// ../version.txt (PROJECT_VER). fw_version() returns that string so the value shown to
// the driver (hello/web) is the SAME one esp_https_ota compares for its "only re-flash
// if different" gate. Bump version.txt per release.
const char *fw_version(void);

// Persisted device settings (NVS). The device is discovered/bound by the Control4
// driver (which connects to us), so only local display preferences live here.
// WiFi creds are owned by the provisioning layer.
typedef struct {
    uint8_t  brightness;       // 0..100 -> backlight (active)
    uint16_t screensaver_sec;  // dim after N idle seconds (0 = never; never while playing)
    uint8_t  dim_brightness;   // 0..100 idle backlight after timeout (0 = screen off; touch wakes)
    uint8_t  orientation;      // 0=landscape 1=portrait 2=landscape180 3=portrait180
    uint8_t  layout;           // 0=Cover 1=Fit 2=Compact
    uint8_t  bg_preset;        // background gradient: 0=Navigator 1=Ocean 2=Dusk 3=Graphite
    uint8_t  theme;            // 0=Control4 (X4)  1=Home Assistant — see ui.c THEME_X4/THEME_HA
    uint8_t  ringer_volume;    // 0..100 chime/announcement/ring loudness (panel speaker)
    uint8_t  muted;            // 0/1 mute the panel's own audio output (forces ringer -> 0)
} settings_t;

extern settings_t g_settings;

void settings_load(void);   // read from NVS (defaults if absent)
void settings_save(void);   // persist to NVS
