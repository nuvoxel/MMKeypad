#pragma once
#include "net.h"

// Now-playing / keypad UI (LVGL). All entry points must run on the LVGL task
// (the caller holds lvgl_port_lock when invoking from another task).
void ui_begin(void);
void ui_splash(void);              // NuVoxel boot splash (call once, right after ui_begin)
void ui_set_state(const media_state_t *st);
void ui_set_connected(bool connected);  // driver TCP link up/down -> placeholder until first state
void ui_request_rebuild(void);     // layout change -> rebuild in place (deferred)
void ui_tick_progress(void);       // ~1s: progress bar + deferred rebuild + pp reconcile
void ui_tick_screensaver(void);    // periodic: dim backlight when idle
void ui_snapshot_start(void);      // dev: LVGL framebuffer-over-serial (MMK_SNAPSHOT only)
void ui_identify(void);            // flash the screen (driver "identify")
void ui_announce(const char *text);  // top banner for a Control4 announcement
// Intercom call screen. `event`: "incoming" | "outgoing" | "active" | "ended"
// (ended hides it). `peer` is the caller's name/AOR (may be NULL). Wakes the
// display. Structured to later host a video region (door camera, P4-class HW
// decode) and a configurable action-button row (e.g. unlock/open gate).
void ui_call(const char *event, const char *peer);

// Callable intercom targets (driver-pushed) for the on-screen Intercom picker.
// intercom_target_t is defined in net.h.
void ui_set_endpoints(const intercom_target_t *eps, int n);
void ui_set_rooms(const room_t *rooms, int n);
void ui_show_rooms_panel(void);   // open the add-rooms panel (sim/preview + programmatic)
void ui_show_intercom_panel(void);   // open the intercom target picker (sim/preview)
// Room navigator favorites grid (driver reply to `getfavorites`). favorite_t is in net.h.
void ui_set_favorites(const favorite_t *favs, int n);

// Sim only: pin the compact bar's rotating text to one phase (0 title, 1 artist,
// 2 album) so a static render can show each. -1 = rotate normally.
extern int g_ui_rot_preview;

// Setup / Wi-Fi onboarding overlay. `ap_name` is the softAP SSID (browser-portal
// fallback). If `pop` is non-NULL, the QR carries the ESP BLE-provisioning payload
// (scan it with the ESP BLE Provisioning app); if NULL, the QR is a plain Wi-Fi
// join code for the softAP. Either way the AP name/instructions are shown too.
void ui_show_setup(const char *ap_name, const char *ap_pass, const char *pop);
void ui_hide_setup(void);
