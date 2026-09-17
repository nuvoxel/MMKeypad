/* Sim stubs -- the non-LVGL surface ui.c links against, replaced with canned,
 * side-effect-free implementations so the shared UI renders standalone on a
 * host. NONE of this touches hardware, the network, NVS, audio or SIP.
 *
 * Only the symbols ui.c actually references are provided (verified by grepping
 * ui.c for net_/art_/device_/bsp_/settings_/fw_version/mmk_read_mac). SIP is
 * compiled out via board.h MMK_HAS_SIP 0, so sip.h supplies its own inline
 * no-ops -- nothing to stub here. device_variant_tag() is a static inline in
 * device.h and needs no stub either. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include "config.h"   /* settings_t, g_settings, settings_save, fw_version */
#include "net.h"
#include "art.h"

/* ── Persisted settings ─────────────────────────────────────────────────────
 * Same defaults the real config.c seeds; render_main overrides fields from env
 * before ui_begin() so you can preview layouts/themes/backgrounds. */
settings_t g_settings = {
    .brightness      = 80,
    .screensaver_sec = 0,
    .dim_brightness  = 10,
    .orientation     = 0,   /* landscape */
    .layout          = 0,   /* Cover */
    .bg_preset       = 0,   /* Navigator */
    .theme           = 0,   /* Control4 X4 */
};
void settings_save(void) {}
void settings_load(void) {}

const char *fw_version(void) { return "sim"; }

/* ── bsp: no-ops (no backlight; rotation is baked into the display size) ───── */
void bsp_set_backlight(uint8_t pct) { (void)pct; }
void bsp_apply_orientation(uint8_t orient) { (void)orient; }

/* ── net: pretend the Control4 driver is connected, canned addresses ──────── */
bool  net_connected(void)              { return true; }
void  net_force_disconnect(void)       { }

/* ── esp_system: no-op -- a static snapshot never actually taps the button,
 * but the linker still needs the symbol Diagnostics' "Reboot panel" calls. */
void  esp_restart(void)                { }
int   net_driver_proto(void)           { return NET_PROTO_VERSION; }
const char *net_peer_ip(void)          { return "192.168.1.220"; }   /* "Director" */
void  net_get_ip(char *buf, size_t n)  { if (buf && n) { strncpy(buf, "192.168.1.50", n - 1); buf[n - 1] = 0; } }
void  net_cmd(const char *c)           { (void)c; }
void  net_set_volume(int level)        { (void)level; }
void  net_send_button(int id)          { (void)id; }
void  net_send_diag(const char *msg)   { (void)msg; }
void  net_request_rooms(void)          { }
void  net_request_favorites(void)      { }
void  net_group_room(const char *id, bool join) { (void)id; (void)join; }
void  net_play_favorite(const char *id)         { (void)id; }
void  net_security_arm(int partition_id, const char *arm_type, const char *pin, bool bypass) { (void)partition_id; (void)arm_type; (void)pin; (void)bypass; }
void  net_security_disarm(int partition_id, const char *pin)      { (void)partition_id; (void)pin; }
void  net_comfort_cmd(int id, const char *action, const char *mode) { (void)id; (void)action; (void)mode; }
void  mmk_read_mac(uint8_t mac[6])     { static const uint8_t m[6] = {0x02,0x00,0x00,0x0C,0x0F,0xEE}; memcpy(mac, m, 6); }

/* ── album art: disabled (no fetch/decode) -- UI shows its placeholder ─────── */
void art_begin(lv_obj_t *p, int x, int y, int w, int h, bool fit) { (void)p;(void)x;(void)y;(void)w;(void)h;(void)fit; }
void art_set_bg(uint32_t top, uint32_t bot, int screen_h)         { (void)top;(void)bot;(void)screen_h; }
void art_load(const char *url)                                    { (void)url; }
void art_tick(void)                                               {}
void art_detach(void)                                             {}
lv_obj_t *art_add_mirror(lv_obj_t *p, int x, int y, int w, int h) { (void)p;(void)x;(void)y;(void)w;(void)h; return NULL; }
bool art_has(void)                                                { return false; }

/* ── device identity / license: canned "licensed Pro S3" so Pro UI shows ──── */
const char *device_hardware_id(void)    { return "sim-0000000000000000"; }
const char *device_secret_hex(void)     { return "00"; }
const char *device_sku_id(void)         { return "mmk-s3"; }   /* -> variant tag "S3" */
const char *device_reg_text(void)       { return "Registered"; }
const char *device_pair_code(void)      { return ""; }
bool        device_is_licensed(void)    { return getenv("MMK_UNLICENSED")?false:true; }
bool        device_is_trial(void)       { return false; }
const char *device_license_label(void)  { return "Pro"; }
bool        device_has_feature(const char *name) { (void)name; return true; }

/* SIP: the sim builds with MMK_HAS_SIP 1 so the intercom UI can be previewed,
 * but there is no stack behind it -- these just swallow the calls. */
void sip_place_call(const char *aor) { (void)aor; }
static bool s_sim_muted;
void sip_set_mute(bool on) { s_sim_muted = on; }
bool sip_is_muted(void) { return s_sim_muted; }
void net_call_mute(bool on) { (void)on; }
void net_call_door(const char *remote, const char *id) { (void)remote; (void)id; }
void sip_answer(void) { }
void sip_hangup(void) { }
const char *device_model_name(void)     { return "MMKeypad Sim"; }
const char *device_mac(void)            { return "02:00:00:0c:0f:ee"; }
const char *device_link_type(void)      { return "ethernet"; }
const char *device_power_source(void)   { return "wall"; }
const char *device_driver_version(void) { return "sim-drv"; }
void        device_ota_check_now(void)  { }

/* Settings now shows Link / Discovery name, and its firmware overlay drives
 * fwupdate.c. Neither belongs in a headless renderer (one wants a live netif, the
 * other HTTPS to GitHub), so stub them with representative values -- the point is
 * to lay out the real strings at the real lengths. */
#include "fwupdate.h"
const char *net_active_transport(void) { return "Ethernet"; }
const char *sddp_host(void)            { return "WS43-e8f60ae4207c"; }

static const fwupdate_rel_t s_sim_rel[] = {
    { .version = "2026.09.06.002", .url = "", .size = 2710192, .current = true  },
    { .version = "2026.09.06.001", .url = "", .size = 2705536, .current = false },
    { .version = "2026.09.03.001", .url = "", .size = 2705536, .current = false },
};
void             fwupdate_start_fetch(void) { }
fwupdate_state_t fwupdate_state(void)       { return FWU_READY; }
int              fwupdate_count(void)       { return (int)(sizeof(s_sim_rel)/sizeof(s_sim_rel[0])); }
const fwupdate_rel_t *fwupdate_get(int i)   { return (i >= 0 && i < fwupdate_count()) ? &s_sim_rel[i] : 0; }
const char      *fwupdate_error(void)       { return ""; }
void             fwupdate_apply(int i)      { (void)i; }

/* ── embedded boot splash PNG ───────────────────────────────────────────────
 * ui_splash() references these asm-named symbols unconditionally (so they must
 * link) but the sim never calls it. A 1-byte dummy is enough -- mirrors the T3
 * platform/stubs.c approach. lodepng_decode32 would just fail and return. */
const uint8_t _sim_splash_dummy_start[1] __asm__("_binary_splash_png_start") = {0};
const uint8_t _sim_splash_dummy_end[1]   __asm__("_binary_splash_png_end")   = {0};

/* The embedded NuVoxel logo (Wi-Fi-setup / claim / settings footer) comes from
 * logo_png.c -- the REAL logo.png bytes (previously a 1-byte dummy, back when
 * the sim never opened those screens; MMK_SETUP=1 in render_main now does).
 * NOTE: lv_snapshot_take() of the top layer does not composite the scaled+
 * recolored image, so the wordmark does not appear in the MMK_SETUP PNG even
 * though it decodes and lays out correctly -- the gradient, title, QR and copy
 * ARE faithful. Confirm the wordmark itself on hardware / the on-device claim
 * screen, which runs the identical logo path. */

/* ── audio: no-ops ─────────────────────────────────────────────────────────
 * ui.c grew ringer-volume / chime / self-test hooks after this sim was last
 * built. The sim renders pixels only, so these just have to link. */
void audio_set_ringer_volume(uint8_t pct) { (void)pct; }
void audio_play_chime(void)               { }
void audio_selftest_async(void)           { }

/* Favourite-tile thumbnails: the sim has no network, so these are no-ops. The
 * tile LAYOUT (art square + title beneath) still renders, which is what the sim
 * is for. */
// Real canvas with a synthetic image, so the sim exercises the SAME draw path the
// firmware uses for favourite artwork (RGB565 canvas + radius + clip_corner) --
// the stub returned NULL, so this path had never been rendered on the host at all.
// A fine checkerboard makes any tearing, stride error or masking artefact obvious.
lv_obj_t *art_thumb_add(lv_obj_t *parent, int x, int y, int w, int h, const char *url)
{
    (void)url;
    if (!parent || w <= 0 || h <= 0) return NULL;
    uint16_t *buf = malloc((size_t)w * h * 2);
    if (!buf) return NULL;
    for (int cy = 0; cy < h; cy++)
        for (int cx = 0; cx < w; cx++) {
            int c = ((cx / 3) + (cy / 3)) & 1;
            /* checker of two strong colours + a diagonal so orientation is visible */
            uint16_t v = c ? 0xF800 : 0x001F;          /* red / blue */
            if (cx == cy) v = 0x07E0;                  /* green diagonal */
            buf[cy * w + cx] = v;
        }
    lv_obj_t *cv = lv_canvas_create(parent);
    lv_canvas_set_buffer(cv, buf, w, h, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(cv, x, y);
    lv_obj_set_style_radius(cv, w / 8, 0);
    lv_obj_set_style_clip_corner(cv, true, 0);
    return cv;
}
void art_thumb_tick(void)  { }
void net_art_debug(int slot, bool ok, int bytes, int w, int h)
{ (void)slot; (void)ok; (void)bytes; (void)w; (void)h; }
void art_thumb_clear(void) { }
void audio_set_user_volume(uint8_t pct) { (void)pct; }
