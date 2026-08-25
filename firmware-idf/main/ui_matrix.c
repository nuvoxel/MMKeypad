// ui_matrix.c — the "UI" for the 64x64 HUB75 album-art board.
//
// It implements the full ui.h contract so main.c stays board-agnostic, but the
// only thing this panel actually shows is the cover art: a single full-screen
// LVGL canvas driven by art.c (the same fetch/decode/scale pipeline the LCD
// boards use). Everything else is a deliberate stub — this is the extension
// surface. Want a clock, a scrolling track title, a VU meter, or a now-playing
// glyph? Add LVGL widgets in ui_begin() and update them from ui_set_state();
// the display, network, and art plumbing are already done.
//
// All entry points run on the LVGL task (callers hold lvgl_port_lock).

#include "ui.h"
#include "art.h"
#include "board.h"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "ui_matrix";
static lv_obj_t *s_setup;   // solid overlay shown during Wi-Fi setup / AP mode

void ui_begin(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Placeholder gradient shown until the first cover art arrives, and used as
    // the letterbox/transparent-icon backdrop by art.c.
    art_set_bg(0x101018, 0x000000, MATRIX_HEIGHT);
    // Full-screen, cover-fill (crop to square) — album art fills all 64x64.
    art_begin(scr, 0, 0, MATRIX_WIDTH, MATRIX_HEIGHT, false);
    ESP_LOGI(TAG, "art canvas %dx%d", MATRIX_WIDTH, MATRIX_HEIGHT);
}

// Now-playing update: hand the art URL to the decoder. "" clears back to the
// gradient. (Title/artist/transport caps are ignored here — no room to show
// them; a future extension could scroll the title.)
void ui_set_state(const media_state_t *st)
{
    if (st) art_load(st->art_url);
}

// Publish freshly decoded art to the canvas (called ~1s from main's tick timer).
void ui_tick_progress(void) { art_tick(); }

// ── Wi-Fi setup indicator ────────────────────────────────────────────────────
// A 64x64 panel can't show a QR/SSID usefully, so setup mode is just a solid
// amber screen: "I'm not on Wi-Fi yet — join my AP and open the portal."
void ui_show_setup(const char *ap_name, const char *ap_pass, const char *pop)
{
    (void)ap_name; (void)ap_pass; (void)pop;
    if (!s_setup) {
        s_setup = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_setup);
        lv_obj_set_size(s_setup, MATRIX_WIDTH, MATRIX_HEIGHT);
        lv_obj_set_pos(s_setup, 0, 0);
        lv_obj_set_style_bg_color(s_setup, lv_color_hex(0xC08000), 0);
        lv_obj_set_style_bg_opa(s_setup, LV_OPA_COVER, 0);
    }
}
void ui_hide_setup(void)
{
    if (s_setup) { lv_obj_delete(s_setup); s_setup = NULL; }
}

// ── Stubs (no visible surface on a 64x64 art panel — extend as desired) ───────
static void identify_off_cb(lv_timer_t *t) { lv_obj_delete((lv_obj_t *)lv_timer_get_user_data(t)); lv_timer_delete(t); }
void ui_identify(void)   // brief full-white flash so "identify" is visible
{
    lv_obj_t *f = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(f);
    lv_obj_set_size(f, MATRIX_WIDTH, MATRIX_HEIGHT);
    lv_obj_set_style_bg_color(f, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
    lv_timer_t *t = lv_timer_create(identify_off_cb, 600, f);
    lv_timer_set_repeat_count(t, 1);
}

void ui_splash(void)                { /* no boot splash on the matrix */ }
void ui_set_connected(bool c)       { (void)c; }
void ui_request_rebuild(void)       { /* fixed layout — nothing to rebuild */ }
void ui_tick_screensaver(void)      { /* backlight/intensity idle-dim: TODO */ }
void ui_snapshot_start(void)        { /* MMK_SNAPSHOT is off on this board */ }
void ui_announce(const char *text)  { (void)text; /* TODO: scroll as a marquee */ }
void ui_call(const char *event, const char *peer) { (void)event; (void)peer; }
void ui_set_endpoints(const intercom_target_t *eps, int n) { (void)eps; (void)n; }
