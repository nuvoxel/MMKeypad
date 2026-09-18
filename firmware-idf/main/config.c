#include "config.h"
#include "nvs.h"
#include "esp_app_desc.h"

// Firmware version string from the app descriptor (set via ../version.txt). Same value
// esp_https_ota uses for its version gate — see config.h.
const char *fw_version(void) { return esp_app_get_description()->version; }

settings_t g_settings = {
    .brightness = 80,
    .screensaver_sec = 30,   // dim the backlight 30s after the last touch (when idle)
    .dim_brightness = 5,     // idle level after the timeout (0 = screen fully off)
    // The 4.3" ws43 wall keypad (480x800) defaults to portrait; the landscape
    // nano keeps 0. Only the compiled
    // default differs — a saved orientation (driver Display Orientation / web UI)
    // still overrides on any board.
#if defined(MMK_BOARD_WS43)
    .orientation = 1,        // portrait
#else
    .orientation = 0,        // landscape
#endif
    .layout = 0,
    .bg_preset = 0,
    .theme = 0,              // Control4 (X4) by default
    .ringer_volume = 80,     // panel chime/announcement loudness
    .muted = 0,              // not muted
    .net_transport = 0,      // Auto: wired first, WiFi fallback
    // Matches driver-keypad/driver.xml's Halo Idle/Call Color + Brightness defaults
    // (index 3 = "Blue" in HALO_PALETTE) so a never-connected panel and a freshly
    // added Composer property agree, instead of the device quietly starting dimmer
    // than what Composer says it's set to.
    .halo_idle_color = 3,    // Blue
    .halo_ring_color = 3,    // Blue
    .halo_brightness = 25,
};

static const char *NS = "mmkeypad";

void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;   // none saved yet -> defaults
    uint8_t u8;
    uint16_t u16;
    if (nvs_get_u8(h, "bright", &u8) == ESP_OK) g_settings.brightness = u8;
    if (nvs_get_u16(h, "ss", &u16) == ESP_OK) g_settings.screensaver_sec = u16;
    if (nvs_get_u8(h, "dimb", &u8) == ESP_OK) g_settings.dim_brightness = u8;
    if (nvs_get_u8(h, "orient", &u8) == ESP_OK) g_settings.orientation = u8;
    if (nvs_get_u8(h, "layout", &u8) == ESP_OK) g_settings.layout = u8;
    if (nvs_get_u8(h, "bg", &u8) == ESP_OK) g_settings.bg_preset = u8;
    if (nvs_get_u8(h, "theme", &u8) == ESP_OK) g_settings.theme = u8;
    if (nvs_get_u8(h, "ringvol", &u8) == ESP_OK) g_settings.ringer_volume = u8;
    if (nvs_get_u8(h, "muted", &u8) == ESP_OK) g_settings.muted = u8;
    if (nvs_get_u8(h, "nettr", &u8) == ESP_OK) g_settings.net_transport = u8;
    if (nvs_get_u8(h, "haloic", &u8) == ESP_OK) g_settings.halo_idle_color = u8;
    if (nvs_get_u8(h, "halorc", &u8) == ESP_OK) g_settings.halo_ring_color = u8;
    if (nvs_get_u8(h, "halobr", &u8) == ESP_OK) g_settings.halo_brightness = u8;
    nvs_close(h);
}

void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "bright", g_settings.brightness);
    nvs_set_u16(h, "ss", g_settings.screensaver_sec);
    nvs_set_u8(h, "dimb", g_settings.dim_brightness);
    nvs_set_u8(h, "orient", g_settings.orientation);
    nvs_set_u8(h, "layout", g_settings.layout);
    nvs_set_u8(h, "bg", g_settings.bg_preset);
    nvs_set_u8(h, "theme", g_settings.theme);
    nvs_set_u8(h, "ringvol", g_settings.ringer_volume);
    nvs_set_u8(h, "muted", g_settings.muted);
    nvs_set_u8(h, "nettr", g_settings.net_transport);
    nvs_set_u8(h, "haloic", g_settings.halo_idle_color);
    nvs_set_u8(h, "halorc", g_settings.halo_ring_color);
    nvs_set_u8(h, "halobr", g_settings.halo_brightness);
    nvs_commit(h);
    nvs_close(h);
}
