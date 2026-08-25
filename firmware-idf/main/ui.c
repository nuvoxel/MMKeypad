#include "ui.h"
#include "device.h"
#include "fwupdate.h"
#include "art.h"
#include "config.h"
#include "bsp.h"
#include "net.h"
#include "sip.h"
#include "audio.h"
#include "lvgl.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lodepng.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Roboto (Control4 / Navigator typeface, Apache-2.0) + DejaVu symbols (♪ ★ ♥ …),
// generated via lv_font_conv. Regular for body, Medium for titles.
extern const lv_font_t mmk_text_14;
extern const lv_font_t mmk_text_16;
extern const lv_font_t mmk_text_24;
extern const lv_font_t mmk_text_32;

// Lucide icon font (MIT) — one consistent open set matched to Control4's vocabulary,
// replacing the built-in FontAwesome symbols. 24px for keypad/transport, 34px play.
extern const lv_font_t mmk_icons_24;
extern const lv_font_t mmk_icons_34;

// Real Control4 X4 icons (A8 alpha masks, mmk_c4icons.c), tinted at render time.
extern const lv_image_dsc_t icon_play, icon_pause, icon_skip_next, icon_skip_prev;
extern const lv_image_dsc_t icon_thumb_up, icon_thumb_down, icon_shuffle;
extern const lv_image_dsc_t icon_vol_up, icon_vol_mute, icon_dots;
extern const lv_image_dsc_t icon_info, icon_close, icon_queue, icon_intercom, icon_call;
extern const lv_image_dsc_t icon_group, icon_door, icon_mic, icon_settings, icon_power;
extern const lv_image_dsc_t icon_light, icon_fan, icon_shade, icon_scene, icon_cool, icon_heat;
extern const lv_image_dsc_t icon_security, icon_camera, icon_speaker, icon_tv, icon_home;
extern const lv_image_dsc_t icon_garage, icon_stop, icon_bell, icon_wifi, icon_heart;
extern const lv_image_dsc_t icon_audio, icon_video, icon_unlock, icon_lock, icon_moon, icon_media;
extern const lv_image_dsc_t icon_repeat, icon_chevron_down, icon_keypad, icon_room_add, icon_back;

// Home Assistant equivalents (mmk_ha_icons.c) — Material Design Icons (Pictogrammers,
// Apache-2.0, generated via tools/icons/gen-ha-icons.sh from @mdi/svg). The C4 bitmaps
// above are Control4's own extracted assets (partner-agreement licensed, see
// mmk_c4icons.c) and aren't appropriate to show under a non-Control4 skin — every
// icon actually used at a fixed UI location (not the driver-driven keypad-button
// name lookup, which already uses the free Lucide font) has an MDI counterpart here.
extern const lv_image_dsc_t icon_ha_media, icon_ha_keypad, icon_ha_intercom, icon_ha_room_add;
extern const lv_image_dsc_t icon_ha_back, icon_ha_chevron_down, icon_ha_power;
extern const lv_image_dsc_t icon_ha_vol_up, icon_ha_vol_mute;
extern const lv_image_dsc_t icon_ha_thumb_up, icon_ha_thumb_down;
extern const lv_image_dsc_t icon_ha_shuffle, icon_ha_repeat, icon_ha_dots, icon_ha_close;
extern const lv_image_dsc_t icon_ha_info, icon_ha_group, icon_ha_door;
extern const lv_image_dsc_t icon_ha_skip_prev, icon_ha_skip_next, icon_ha_play, icon_ha_pause;
// Theme-aware icon pick — one macro per logical icon, used everywhere the C4 bitmap
// used to be referenced directly. g_settings.theme is checked directly (not g_theme,
// which carries colors only) since it's read at call time, not just at rebuild.
#define UI_HA (g_settings.theme == 1)
#define ICON_MEDIA        (UI_HA ? &icon_ha_media        : &icon_media)
#define ICON_KEYPAD       (UI_HA ? &icon_ha_keypad       : &icon_keypad)
#define ICON_INTERCOM     (UI_HA ? &icon_ha_intercom     : &icon_intercom)
// Phone mark for mobile/remote endpoints. No HA variant exists, so both themes
// use the Control4 glyph rather than shipping a mismatched stand-in.
#define ICON_CALL         (&icon_call)
#define ICON_ROOM_ADD     (UI_HA ? &icon_ha_room_add     : &icon_room_add)
#define ICON_BACK         (UI_HA ? &icon_ha_back         : &icon_back)
#define ICON_CHEVRON_DOWN (UI_HA ? &icon_ha_chevron_down : &icon_chevron_down)
#define ICON_POWER        (UI_HA ? &icon_ha_power        : &icon_power)
#define ICON_VOL_UP       (UI_HA ? &icon_ha_vol_up       : &icon_vol_up)
#define ICON_VOL_MUTE     (UI_HA ? &icon_ha_vol_mute     : &icon_vol_mute)
#define ICON_THUMB_UP     (UI_HA ? &icon_ha_thumb_up     : &icon_thumb_up)
#define ICON_THUMB_DOWN   (UI_HA ? &icon_ha_thumb_down   : &icon_thumb_down)
#define ICON_SHUFFLE      (UI_HA ? &icon_ha_shuffle      : &icon_shuffle)
#define ICON_REPEAT       (UI_HA ? &icon_ha_repeat       : &icon_repeat)
#define ICON_DOTS         (UI_HA ? &icon_ha_dots         : &icon_dots)
#define ICON_CLOSE        (UI_HA ? &icon_ha_close        : &icon_close)
#define ICON_INFO         (UI_HA ? &icon_ha_info         : &icon_info)
#define ICON_GROUP        (UI_HA ? &icon_ha_group        : &icon_group)
#define ICON_DOOR         (UI_HA ? &icon_ha_door         : &icon_door)
#define ICON_SKIP_PREV    (UI_HA ? &icon_ha_skip_prev    : &icon_skip_prev)
#define ICON_SKIP_NEXT    (UI_HA ? &icon_ha_skip_next    : &icon_skip_next)
#define ICON_PLAY         (UI_HA ? &icon_ha_play         : &icon_play)
#define ICON_PAUSE        (UI_HA ? &icon_ha_pause        : &icon_pause)

// Resolution-aware font roles. The fixed bitmap sizes above were tuned for a
// mid-size ESP panel (~480px short side); on larger panels (P4 720, T3 800+)
// they render small. So the F*/FICON* names are now runtime pointers, chosen
// per display in ui_apply_font_scale() below. Default to the tuned bitmaps so
// small/mid boards (and any build with LV_USE_TINY_TTF off) are byte-identical.
static const lv_font_t *g_fSmall  = &mmk_text_14;   // captions, time, info rows
static const lv_font_t *g_fBody   = &mmk_text_16;   // body / secondary
static const lv_font_t *g_fTitle  = &mmk_text_24;   // now-playing title (Medium)
static const lv_font_t *g_fHead   = &mmk_text_32;   // big / clock (Medium)
static const lv_font_t *g_fIcon   = &mmk_icons_24;
static const lv_font_t *g_fIconL  = &mmk_icons_34;
#define F14 (g_fSmall)
#define F16 (g_fBody)
#define F24 (g_fTitle)
#define F32 (g_fHead)
#define FICON  (g_fIcon)
#define FICONL (g_fIconL)

// Glyphs (UTF-8) used directly by the transport/overlay UI.
#define G_PLAY  "\xEE\x84\xBC"   // play
#define G_PAUSE "\xEE\x84\xAE"   // pause
#define G_PREV  "\xEE\x85\x9F"   // skip-back
#define G_NEXT  "\xEE\x85\xA0"   // skip-forward
#define G_LIST  "\xEE\x84\x86"   // list
#define G_INFO  "\xEE\x83\xB9"   // info
#define G_VOL   "\xEE\x86\xAB"   // volume-2
#define G_MUTE  "\xEE\x86\xAC"   // volume-x
#define G_CLOSE "\xEE\x86\xB2"   // x
#define G_STOP  "\xEE\x85\xA7"   // square (stop)
#define G_THUP  "\xEE\x86\x8A"   // thumbs-up
#define G_THDN  "\xEE\x86\x89"   // thumbs-down
#define G_DOTS  "\xEE\x82\xB7"   // ellipsis-vertical (⋮ overflow, like Navigator)
#define G_PHONE "\xEE\x84\xB3"   // phone (intercom entry / room target)
#define G_CALL  "\xEE\x84\xB4"   // phone-call
#define G_USERS "\xEE\x86\xA4"   // users (group / broadcast target)
#define G_DOOR  "\xEE\x8F\x96"   // door-open (door-station target)
#define G_MIC   "\xEE\x84\x98"   // mic

// Friendly icon name (driver's "Button N Icon" list) -> Lucide glyph.
typedef struct { const char *name, *glyph; } icon_map_t;
static const icon_map_t ICONS[] = {
    {"Play",G_PLAY},{"Pause",G_PAUSE},{"Next",G_NEXT},{"Previous",G_PREV},{"Stop","\xEE\x85\xA7"},
    {"Shuffle","\xEE\x85\x9E"},{"Loop","\xEE\x85\x86"},{"Volume",G_VOL},{"Mute",G_MUTE},
    {"List",G_LIST},{"Info",G_INFO},{"Settings","\xEE\x85\x94"},{"Power","\xEE\x85\x80"},
    {"Lights","\xEE\x87\x82"},{"Fan","\xEE\x8D\xB9"},{"Shade","\xEE\x8F\x80"},{"Scene","\xEE\x90\x92"},
    {"Climate","\xEE\x86\x86"},{"Cool","\xEE\x85\xA5"},{"Heat","\xEE\x83\x92"},{"Security","\xEE\x85\x98"},
    {"Lock","\xEE\x84\x8B"},{"Unlock","\xEE\x84\x8C"},{"Camera","\xEE\x81\xA4"},{"Media","\xEE\x84\xA2"},
    {"Audio","\xEE\x95\x9A"},{"Video","\xEE\x86\xA5"},{"TV","\xEE\x86\x95"},{"Speaker","\xEE\x85\xA6"},
    {"Home","\xEE\x83\xB5"},{"Bell","\xEE\x81\x99"},{"Wifi","\xEE\x86\xAE"},{"Sun","\xEE\x85\xB8"},
    {"Moon","\xEE\x84\x9E"},{"Star","\xEE\x85\xB6"},{"Heart","\xEE\x83\xB2"},{"Plug","\xEE\x8D\xBF"},
    {"Door","\xEE\x8F\x96"},{"Garage","\xEE\x8F\xA6"},
};
static const char *iconGlyph(const char *n)
{
    for (size_t i = 0; i < sizeof(ICONS) / sizeof(ICONS[0]); i++)
        if (!strcmp(n, ICONS[i].name)) return ICONS[i].glyph;
    return NULL;
}

// ── Theme tokens ─────────────────────────────────────────────────────────────
// The layout is theme-agnostic: it reads all colors from the ACTIVE theme, so a
// non-Control4 skin (e.g. Home Assistant colors) is just a second theme_t with the
// SAME layout. The palette macros below resolve through g_theme, so every existing
// call site (lv_color_hex(C_TEXT) etc.) is theme-driven with no change.
typedef struct {
    uint32_t bg_top, bg_bot;   // home / now-playing shared gradient
    uint32_t text;             // primary text (c4White)
    uint32_t subtle;           // secondary text (c4Silver)
    uint32_t muted;            // tertiary text (c4Grey)
    uint32_t accent;           // signature accent (c4Blue)
    uint32_t on;               // active/on state (c4GreenBright)
    uint32_t danger;           // decline / end call (c4Red)
    uint32_t chip;             // elevated chip/card surface
    uint32_t btn;              // button surface
} theme_t;

// Default theme: Control4 X4 / Navigator palette (extracted from C4UIKit Assets.car).
static const theme_t THEME_X4 = {
    .bg_top = 0x312B63, .bg_bot = 0x235E97,
    .text = 0xFFFFFF, .subtle = 0xDCDEE0, .muted = 0x9C9C9C,
    .accent = 0x32B4E5, .on = 0x00BD00, .danger = 0xE0211D,
    .chip = 0x242424, .btn = 0x404040,
};
// Home Assistant dark-frontend palette: near-flat near-black (HA doesn't do C4's
// purple/blue gradient), HA blue accent, Material red/green for danger/on.
static const theme_t THEME_HA = {
    .bg_top = 0x1C1C1C, .bg_bot = 0x111111,
    .text = 0xFFFFFF, .subtle = 0xADADAD, .muted = 0x727272,
    .accent = 0x03A9F4, .on = 0x4CAF50, .danger = 0xF44336,
    .chip = 0x262626, .btn = 0x333333,
};
// Active theme pointer — selected from g_settings.theme (see ui_apply_theme).
static const theme_t *g_theme = &THEME_X4;
static void ui_apply_theme(void) { g_theme = (g_settings.theme == 1) ? &THEME_HA : &THEME_X4; }

#define HOME_BG_TOP (g_theme->bg_top)
#define HOME_BG_BOT (g_theme->bg_bot)
#define C_TEXT   (g_theme->text)
#define C_SUBTLE (g_theme->subtle)
#define C_MUTED  (g_theme->muted)
#define C_ACCENT (g_theme->accent)
#define C_GREEN  (g_theme->on)
#define C_RED    (g_theme->danger)
#define C_CHIP   (g_theme->chip)
#define C_BTN    (g_theme->btn)

// X4 "glass" card treatment: a translucent black fill + a hairline light
// border over the background, matching Navigator's surface look (built from the
// c4Black/c4White opacity ramps in the asset catalog). glassify() keeps every
// card consistent; radius scales with the display so cards look right on any
// panel. Used for list rows, panels, and overlays.
static inline void glassify(lv_obj_t *o, int radius) {
    lv_obj_set_style_bg_color(o, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_40, 0);                 // c4BlackOpacity40 glass
    lv_obj_set_style_bg_opa(o, LV_OPA_60, LV_STATE_PRESSED);  // darken on press
    lv_obj_set_style_border_color(o, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(o, LV_OPA_20, 0);             // c4WhiteOpacity20 hairline
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_radius(o, radius, 0);
}

static uint32_t millis(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static lv_obj_t *s_scrim, *s_title, *s_artist, *s_time, *s_bar, *s_ppIcon;
static lv_obj_t *s_timeTot;   // X4: total-time label at the progress bar's right end
static lv_obj_t *s_play, *s_prev, *s_next, *s_thumbUp, *s_thumbDown, *s_shufBtn, *s_repBtn;
static lv_obj_t *s_shufIcon, *s_repIcon;      // shuffle/repeat glyphs (for on/off retint)
static bool      s_x4;                        // large-landscape X4 card layout (T3/nano)
static lv_obj_t *s_npCard, *s_npVol, *s_npVolIcon;  // X4 now-playing card + its persistent volume
static lv_obj_t *s_roomsPanel, *s_roomsVol, *s_roomsVolIcon, *s_roomsList;  // X4 add-rooms right panel
static lv_obj_t *s_npRoomLbl, *s_roomsRowLbl;   // "Playing in <room>" + the room-list row name
static bool      s_stopMode;                 // primary button is Stop (non-pauseable source)
static int       s_trCx, s_trCy, s_trPlay, s_trSide, s_trMaxW;   // transport row geometry (for adaptive layout)
static bool      s_cPrev = true, s_cNext = true, s_cThUp = false, s_cThDn = false;  // visible-button caps
static bool      s_cPrimary = true;          // center play/pause/stop shown (source has any transport)
static bool      s_cShuffle, s_cRepeat;      // source supports shuffle/repeat
static bool      s_shufOn, s_repOn;          // current on/off (authoritative from driver)
static uint32_t  s_shufTapMs, s_repTapMs;    // last local tap: hold optimistic state briefly
static lv_obj_t *s_infoPanel;
static lv_obj_t *s_infoRows[8];                 // per-field row container
static lv_obj_t *s_infoName[8], *s_infoVal[8];  // field name (gray) + value (white)
static lv_obj_t *s_infoToggle;
// Intercom picker: full-screen list of callable targets (groups + endpoints), driver-pushed.
static lv_obj_t *s_icPanel, *s_icList, *s_icToggle;
static intercom_target_t s_eps[24];
static int       s_nEps;
static bool      s_haveButtons;
// Multiroom add-rooms list, kept so a row tap can join/leave that room (write side).
static room_t    s_rooms[NET_MAX_ROOMS];
static int       s_nRooms;
// Room navigator favorites: full-screen tile grid, driver-pushed on getfavorites. Kept
// so a tile tap can resolve its id -> net_play_favorite (write side).
static lv_obj_t *s_favPanel, *s_favGrid;
static favorite_t s_favs[NET_MAX_FAVORITES];
static int       s_nFavs;

// Intercom availability is purely a hardware fact: the panel shows the intercom
// feature iff the board has SIP audio. It deliberately does NOT depend on s_nEps
// (the driver-pushed call-target list) — the intercom screen itself renders
// "No intercom targets" while s_nEps == 0, so an unconfigured driver just shows
// an empty target list rather than hiding the feature entirely.
static inline bool ic_available(void)
{
    return MMK_HAS_SIP != 0;
}
// X4-inspired home/main screen (modern, not a C4 clone): room name + a few big
// glass cards (Listen/Intercom/Keypad) + a mini-player bar. Shown by default;
// tapping Listen / the mini-player expands into the now-playing screen.
static lv_obj_t *s_home, *s_homeMiniTitle, *s_homeMiniArtist, *s_homeMiniArt, *s_homeMiniPP;
static lv_obj_t *s_homeVol, *s_homeVolIcon;   // persistent volume on the home mini-player
static lv_obj_t *s_homeMiniNote;              // music-note placeholder over the art tile (no art)
static lv_obj_t *s_homeBar;                   // mini-player bar (hidden when the room is off)
static bool      s_homeIcShown;               // home Intercom card is currently shown (intercom available)
static bool      s_homeAtHome = true;   // start on the home screen
static lv_obj_t *s_homeTitle;           // landing-page title (the room name)
static bool      s_hadSession;          // was a media session active on the last state?
// Keypad grid: up to MMK_MAX_BUTTONS driver buttons (a per-board constant — the
// panel's geometry decides, see board.h), plus (in Keypad-primary mode) 2 synthetic
// tiles — Listen + Intercom — appended after the driver buttons.
static media_state_t s_lastState;
static char s_lastHead[96] = {0}, s_lastSub[192] = {0};   // #15: skip redundant title/artist re-sets
static bool      s_haveState;
static bool      s_connected;   // driver TCP link up (for the boot placeholder)
// UI layout scale, matched to the resolution-aware font scale (1.0 unless the
// panel is large enough to scale fonts up, i.e. tiny_ttf boards like the T3).
// ui_begin() scales control sizes + metadata positions by this so they don't
// stay tiny / overlap when the type grows on a big display.
static float     s_uiscale = 1.0f;
static volatile bool s_rebuildReq;
static bool      s_lastPower = true;
static bool      s_showTitle = true, s_showArtist = true, s_showInfo = true, s_showProgress = true;

static bool s_lastPlaying;
static bool s_ppExpect;
static uint32_t s_ppHoldUntil;
static lv_obj_t *s_call;   // intercom call overlay (NULL when no call); keeps display lit
static bool s_keepAwake;
static uint32_t s_loadUntil;

static int s_elapsed, s_duration;
static char s_progTitle[96];
static uint32_t s_progMs;

static void setVis(lv_obj_t *o, bool show)
{
    if (!o) return;
    if (show) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// Place the transport controls the source actually supports, centered as one row:
// [shuffle] [thumbDown] [prev] [play/stop] [next] [thumbUp] [repeat]. Hidden ones
// are skipped, so e.g. SiriusXM shows just Stop and Pandora shows 👎 ⏸ ⏭ 👍.
static void layoutTransport(void)
{
    lv_obj_t *all[7] = { s_shufBtn, s_thumbDown, s_prev, s_play, s_next, s_thumbUp, s_repBtn };
    for (int i = 0; i < 7; i++) setVis(all[i], false);
    lv_obj_t *row[7]; int n = 0;
    if (s_cShuffle) row[n++] = s_shufBtn;
    if (s_cThDn) row[n++] = s_thumbDown;
    if (s_cPrev) row[n++] = s_prev;
    if (s_cPrimary) row[n++] = s_play;   // hidden for no-transport sources (e.g. a computer/Mac)
    if (s_cNext) row[n++] = s_next;
    if (s_cThUp) row[n++] = s_thumbUp;
    if (s_cRepeat) row[n++] = s_repBtn;
    if (n == 0) return;
    int gap = 10, side = s_trSide, play = s_trPlay;
    // Scale the row down to fit the usable width, else a full 7-control row (shuffle
    // + thumbs + skip + play + repeat) runs off both edges in portrait.
    int maxw = (s_trMaxW > 0 ? s_trMaxW : 320) - 8;
    int total = 0;
    for (int i = 0; i < n; i++) total += (row[i] == s_play ? play : side) + (i ? gap : 0);
    if (total > maxw) {
        float sc = (float)maxw / total;
        gap  = (int)(gap  * sc); if (gap  < 4)  gap  = 4;
        side = (int)(side * sc); if (side < 30) side = 30;
        play = (int)(play * sc); if (play < 40) play = 40;
        total = 0;
        for (int i = 0; i < n; i++) total += (row[i] == s_play ? play : side) + (i ? gap : 0);
    }
    int x = s_trCx - total / 2;
    if (x < 4) x = 4;   // never start off-screen
    for (int i = 0; i < n; i++) {
        int sz = (row[i] == s_play ? play : side);
        lv_obj_set_size(row[i], sz, sz);
        lv_obj_set_pos(row[i], x, s_trCy - sz / 2);
        setVis(row[i], true);
        x += sz + gap;
    }
}

// Load-dim removed: darkening the whole screen on track-change read as a
// jarring flash. The transport tap is feedback enough, and the old art simply
// stays until the new cover is decoded (art.c keeps it), then swaps in.
static void setDim(bool on) { (void)on; }
static void startLoading(void) { s_loadUntil = millis() + 1500; }

// ── event handlers ──────────────────────────────────────────────────────────
static void onPrev(lv_event_t *e) { (void)e; net_cmd("prev"); startLoading(); }
static void onNext(lv_event_t *e) { (void)e; net_cmd("next"); startLoading(); }
static void onThumbUp(lv_event_t *e)   { (void)e; net_cmd("thumbsup"); }
static void onThumbDown(lv_event_t *e) { (void)e; net_cmd("thumbsdown"); }

// Shuffle/repeat are toggles, living as two more optional slots in the transport
// row (plain glyphs, like thumbs up/down — retinted on/off rather than pill-styled,
// since the row has no background to show a bg-color toggle against). The driver
// reports the real state (from the digital-audio session), but it lags a poll
// behind a tap, so we flip optimistically on tap and hold that for ~2s before
// letting the authoritative state reconcile (see ui_set_state).
static void styleToggle(lv_obj_t *icon, bool on)
{
    if (icon) lv_obj_set_style_image_recolor(icon, lv_color_hex(on ? C_GREEN : 0xFFFFFF), 0);
}
static void onShuffle(lv_event_t *e) { (void)e; s_shufOn = !s_shufOn; s_shufTapMs = millis(); styleToggle(s_shufIcon, s_shufOn); net_cmd("shuffle"); }
static void onRepeat(lv_event_t *e)  { (void)e; s_repOn  = !s_repOn;  s_repTapMs  = millis(); styleToggle(s_repIcon,  s_repOn);  net_cmd("repeat"); }
static void onPlayPause(lv_event_t *e)
{
    (void)e;
    if (s_stopMode) { net_cmd("stop"); startLoading(); return; }   // non-pauseable source
    s_lastPlaying = !s_lastPlaying;
    s_ppExpect = s_lastPlaying;
    s_ppHoldUntil = millis() + 3000;
    if (s_ppIcon) lv_image_set_src(s_ppIcon, s_lastPlaying ? ICON_PAUSE : ICON_PLAY);
    if (s_homeMiniPP) lv_image_set_src(s_homeMiniPP, s_lastPlaying ? ICON_PAUSE : ICON_PLAY);
    net_cmd(s_lastPlaying ? "play" : "pause");
}
// X4 card's persistent volume slider (no pop-up overlay).
static void onNpVol(lv_event_t *e)
{
    if (!s_npVol) return;
    int v = lv_slider_get_value(s_npVol);
    if (s_npVolIcon) lv_image_set_src(s_npVolIcon, v == 0 ? ICON_VOL_MUTE : ICON_VOL_UP);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) net_set_volume(v);
}
// X4 card right panel: exactly one of {info, rooms} is visible (or neither).
static void npShowRight(int which)   // 1=info, 2=rooms, 0=none
{
    setVis(s_infoPanel, which == 1);
    if (s_roomsPanel) setVis(s_roomsPanel, which == 2);
}
// Open the add-rooms panel programmatically. Used by the headless sim to render
// this screen for layout work (it is otherwise only reachable by a tap).
void ui_show_rooms_panel(void)
{
    // Same sequence the UI takes: leave Home for the now-playing card, then raise
    // the rooms panel over it.
    s_homeAtHome = false;
    if (s_home) lv_obj_add_flag(s_home, LV_OBJ_FLAG_HIDDEN);
    npShowRight(2);
}

// Portrait X4: the info/rooms panels are full-card overlays (no side-by-side room
// for a persistent close button), so a tap on the panel dismisses it back to the card.
static void onRoomsClose(lv_event_t *e) { (void)e; if (s_roomsPanel) lv_obj_add_flag(s_roomsPanel, LV_OBJ_FLAG_HIDDEN); }
// Add-rooms master volume = the GROUP volume (all grouped rooms together), which is
// a DIFFERENT level from this room's own volume. Grouping doesn't exist yet, so for
// now it falls back to this room's volume (net_set_volume). TODO: when the driver
// supports session grouping, send a group-volume command here instead.
static void onRoomsVol(lv_event_t *e)
{
    if (!s_roomsVol) return;
    int v = lv_slider_get_value(s_roomsVol);
    if (s_roomsVolIcon) lv_image_set_src(s_roomsVolIcon, v == 0 ? ICON_VOL_MUTE : ICON_VOL_UP);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) net_set_volume(v);   // group vol TBD
}
static void onInfo(lv_event_t *e)
{
    (void)e;
    if (!s_infoPanel) return;
    bool opening = lv_obj_has_flag(s_infoPanel, LV_OBJ_FLAG_HIDDEN);
    if (opening) lv_obj_clear_flag(s_infoPanel, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(s_infoPanel, LV_OBJ_FLAG_HIDDEN);
    if (s_x4 && opening && s_roomsPanel) lv_obj_add_flag(s_roomsPanel, LV_OBJ_FLAG_HIDDEN);  // info replaces rooms
}
static void expandListen(lv_event_t *e);   // → now-playing
static void homeIntercom(lv_event_t *e);   // → intercom picker


// Circular button carrying a C4 image icon (recolored + scaled to fit), the
// image-based twin of iconBtn(). The icon is ~60% of the button; A8 masks take
// iconColor as their fill so the same asset works white on art or accent when
// active. imgOut returns the image obj (swap its src for play/pause).
static lv_obj_t *iconBtnImg(lv_obj_t *parent, const lv_image_dsc_t *img, int size,
                            uint32_t bg, lv_opa_t bgOpa, uint32_t iconColor,
                            lv_event_cb_t cb, lv_obj_t **imgOut)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, size / 2, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(b, bgOpa, 0);
    lv_obj_t *im = lv_image_create(b);
    lv_image_set_src(im, img);
    int iconPx = size * 3 / 5;
    lv_image_set_scale(im, iconPx * 256 / (int)img->header.w);
    lv_obj_set_style_image_recolor(im, lv_color_hex(iconColor), 0);
    lv_obj_set_style_image_recolor_opa(im, LV_OPA_COVER, 0);
    lv_obj_center(im);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    if (imgOut) *imgOut = im;
    return b;
}

// Drop characters the built-in font can't render (no missing-glyph boxes).
static void sanitize(const char *in, char *out, size_t outsz)
{
    size_t oi = 0, i = 0, n = in ? strlen(in) : 0;
    while (i < n && oi + 4 < outsz) {
        uint8_t c = (uint8_t)in[i];
        int len = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        if (i + len > n) break;
        uint32_t cp;
        if (len == 1) cp = c;
        else { cp = c & (0xFF >> (len + 1)); for (int j = 1; j < len; j++) cp = (cp << 6) | ((uint8_t)in[i + j] & 0x3F); }
        // Explicit-content badge (🄴/🅴/Ⓔ) — no glyph in any bundled font; show "[E]".
        if (cp == 0x1F134 || cp == 0x1F174 || cp == 0x1F132 || cp == 0x24BA) {
            if (oi + 3 < outsz) { out[oi++] = '['; out[oi++] = 'E'; out[oi++] = ']'; }
            i += len;
            continue;
        }
        lv_font_glyph_dsc_t dsc;
        if (cp == ' ' || (cp > 0 && lv_font_get_glyph_dsc(F24, &dsc, cp, 0))) {
            for (int j = 0; j < len; j++) out[oi++] = in[i + j];
        }
        i += len;
    }
    out[oi] = 0;
}


static lv_obj_t *mkLabel(lv_obj_t *p, const lv_font_t *f, uint32_t color, int w, bool scroll)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_long_mode(l, scroll ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_DOT);
    if (w) lv_obj_set_width(l, w);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

// ── NuVoxel boot splash ──────────────────────────────────────────────────────
// Full-screen NuVoxel logo (embedded splash.png, decoded via lodepng) shown on top of
// the UI for ~1.8s at boot, then removed to reveal the now-playing screen.
// NOTE: doRebuild() (further down) also tears these down -- a rebuild inside the splash
// window frees the canvas via lv_obj_clean(), and splash_done() must not delete it again.
static lv_obj_t *s_splashCv;
static uint16_t *s_splashBuf;
static void splash_done(lv_timer_t *t)
{
    lv_timer_delete(t);
    if (s_splashCv)  { lv_obj_delete(s_splashCv); s_splashCv = NULL; }
    if (s_splashBuf) { heap_caps_free(s_splashBuf); s_splashBuf = NULL; }
}
void ui_splash(void)
{
    extern const uint8_t splash_png_start[] asm("_binary_splash_png_start");
    extern const uint8_t splash_png_end[]   asm("_binary_splash_png_end");
    unsigned char *rgba = NULL; unsigned iw = 0, ih = 0;
    if (lodepng_decode32(&rgba, &iw, &ih, splash_png_start,
                         (size_t)(splash_png_end - splash_png_start)) != 0 || !rgba) {
        if (rgba) free(rgba);
        return;
    }
    lv_obj_t *scr = lv_screen_active();
    int Wd = lv_obj_get_width(scr), Hd = lv_obj_get_height(scr);
    s_splashBuf = heap_caps_malloc((size_t)Wd * Hd * 2, MALLOC_CAP_SPIRAM);
    if (!s_splashBuf) { free(rgba); return; }
    for (int i = 0; i < Wd * Hd; i++) s_splashBuf[i] = 0xFFFF;      // white field
    int ox = (Wd - (int)iw) / 2, oy = (Hd - (int)ih) / 2;          // center the logo
    for (int y = 0; y < (int)ih; y++) {
        int dy = oy + y; if (dy < 0 || dy >= Hd) continue;
        for (int x = 0; x < (int)iw; x++) {
            int dx = ox + x; if (dx < 0 || dx >= Wd) continue;
            const unsigned char *p = &rgba[((size_t)y * iw + x) * 4];
            unsigned a = p[3];                                     // composite over white
            unsigned r = (p[0] * a + 255u * (255 - a)) / 255;
            unsigned g = (p[1] * a + 255u * (255 - a)) / 255;
            unsigned b = (p[2] * a + 255u * (255 - a)) / 255;
            s_splashBuf[dy * Wd + dx] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
    free(rgba);
    s_splashCv = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_splashCv, s_splashBuf, Wd, Hd, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_splashCv, 0, 0);
    lv_obj_move_foreground(s_splashCv);
    lv_timer_create(splash_done, 1800, NULL);
}

// ── Intercom picker ──────────────────────────────────────────────────────────
static void showHome(void);   // fwd: home/keypad/intercom card navigation
static void onIcRow(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= 0 && i < s_nEps) sip_place_call(s_eps[i].user, s_eps[i].name);   // outbound call (Calling… UI follows)
    if (s_icPanel) lv_obj_add_flag(s_icPanel, LV_OBJ_FLAG_HIDDEN);
    showHome();
}
// X4: close a full card (keypad/intercom) back to the home screen.
static void goHomeFrom(lv_obj_t *panel) { if (panel) lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN); showHome(); }
static void onIcHome(lv_event_t *e) { (void)e; goHomeFrom(s_icPanel); }
// Defined further down with the room page; the intercom picker uses the same tile
// so the two pages share one visual language.
static lv_obj_t *tileCard(lv_obj_t *p, const lv_image_dsc_t *icon, const char *glyph,
                          const char *name, const char *sub, bool on, uint32_t accent,
                          lv_event_cb_t cb, void *ud, int w, int h, lv_obj_t **iconOut,
                          int *iconX, int *iconSize);

static void rebuildIcList(void)
{
    if (!s_icList) return;
    lv_obj_clean(s_icList);                                    // drop old rows
    const float s = s_uiscale;
    const int gap = (int)(14 * s);
    // Derive the usable width rather than reading it back: rebuildIcList runs from
    // ui_begin before any layout pass, where lv_obj_get_width() is still 0 -- which
    // sized every tile to nothing and rendered an empty page.
    const int dispW = lv_display_get_horizontal_resolution(NULL);
    const bool portrait = (lv_display_get_vertical_resolution(NULL) > dispW);
    const int icPad = (s < 0.99f) ? 12 : (int)(28 * s);   // matches s_icList's inset
    const int W = dispW - 2 * icPad;
    // Same column rule as the room page (raw-pixel targets). This was a
    // single full-width column of dividers, which on the 10" showed five of
    // eight targets and left two thirds of the panel empty.
    int cols = portrait ? ((W + gap) / (190 + gap)) : ((W + gap) / (340 + gap));
    if (cols < 1) cols = 1;
    if (cols > 4) cols = 4;
    if (portrait && cols > 2) cols = 2;
    const int cw = (W - (cols - 1) * gap) / cols;
    const int ch = (int)(76 * s);

    lv_obj_set_flex_flow(s_icList, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(s_icList, gap, 0);
    lv_obj_set_style_pad_row(s_icList, gap, 0);

    for (int i = 0; i < s_nEps; i++) {
        // Doors are the targets that ring YOU, so they carry the accent; groups
        // and rooms are plain. Previously each of the three kinds had its own
        // icon colour (blue door, green group, white room), which read as
        // arbitrary rather than meaningful.
        const bool door   = s_eps[i].door;
        const bool mobile = s_eps[i].mobile;
        const lv_image_dsc_t *ei = s_eps[i].group ? ICON_GROUP
                                 : (door ? ICON_DOOR
                                 : (mobile ? ICON_CALL : ICON_INTERCOM));
        // A plain room gets the Lucide speaker mark: the intercom image icon is a
        // dotted broadcast burst that reads as noise at tile size next to the
        // crisp door and group marks. A mobile is a PERSON, not a room, so it gets
        // the phone mark -- the Control4 app draws these the same way.
        const char *gi = door ? "Door" : (mobile ? NULL : ((!s_eps[i].group) ? "Speaker" : NULL));
        const char *sub = door ? "Door station"
                        : (mobile ? "Mobile" : (s_eps[i].group ? "Group" : NULL));
        // Tinted like a door station so the non-room entries stand out, but in a
        // different colour so the two are not confused at a glance.
        tileCard(s_icList, ei, gi, s_eps[i].name, sub, door || mobile,
                 mobile ? C_GREEN : C_ACCENT,
                 onIcRow, (void *)(intptr_t)i, cw, ch, NULL, NULL, NULL);
    }
    if (s_nEps == 0) {
        lv_obj_t *l = lv_label_create(s_icList);
        lv_obj_set_style_text_font(l, F16, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0x9AA5B1), 0);
        lv_label_set_text(l, "No intercom targets");
        lv_obj_set_style_pad_top(l, (int)(20 * s_uiscale), 0);
    }
}
// A room row was tapped: join or leave that room's grouping with ours (Stage 4 write
// side). Send the write, then re-request the list so the driver's settled membership
// re-renders the dots. Our own room is the session anchor and can't be un-grouped from
// here — guard it by name match against the now-playing room.
static void onRoomRow(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_nRooms) return;
    if (s_rooms[i].grouped && s_lastState.room[0] &&
        !strcmp(s_rooms[i].name, s_lastState.room)) return;
    net_group_room(s_rooms[i].id, !s_rooms[i].grouped);
    net_request_rooms();   // driver re-enumerates var 1006 → refreshed rooms list
}

// Multiroom "add-rooms" list from the driver (`rooms` reply to getrooms). Rebuild
// the panel's scrollable list. Styled after the Control4 app's room list: EVERY
// row carries a selection circle on the right -- hollow when the room is not in
// your session, filled when it is -- and a small speaker mark sits left of the
// name on rooms that are playing. It used to show a bright green filled dot (and
// amber for playing), which read as a status LED rather than a checkbox and only
// appeared on some rows, so there was no column to scan. Tapping a row
// joins/leaves that room's grouping (onRoomRow → net_group_room). Called under
// the LVGL lock from on_net_rooms.
void ui_set_rooms(const room_t *rooms, int n)
{
    if (!s_roomsList) return;
    if (n < 0) n = 0;
    if (n > NET_MAX_ROOMS) n = NET_MAX_ROOMS;
    if (n > 0) memcpy(s_rooms, rooms, (size_t)n * sizeof(room_t));
    s_nRooms = n;
    s_roomsRowLbl = NULL;              // the stub "this room" row is about to be deleted
    lv_obj_clean(s_roomsList);
    const float s = s_uiscale;
    for (int i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_create(s_roomsList);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, (int)(54 * s));
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_opa(row, LV_OPA_20, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, onRoomRow, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        // Speaker mark for a room that is playing, left of its name (the C4 app
        // puts one there). Purely informational -- the circle on the right is the
        // membership control.
        int lx = 0;
        if (rooms[i].playing) {
            const int spk = (int)(20 * s);
            lv_obj_t *sp = lv_image_create(row);
            lv_image_set_src(sp, ICON_VOL_UP);
            lv_obj_set_size(sp, spk, spk);
            lv_image_set_inner_align(sp, LV_IMAGE_ALIGN_CONTAIN);
            lv_obj_set_style_image_recolor(sp, lv_color_hex(C_TEXT), 0);
            lv_obj_set_style_image_recolor_opa(sp, LV_OPA_70, 0);
            lv_obj_align(sp, LV_ALIGN_LEFT_MID, 0, 0);
            lx = spk + (int)(10 * s);
        }

        lv_obj_t *lbl = lv_label_create(row);
        lv_obj_set_style_text_font(lbl, F16, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), 0);
        lv_obj_set_style_text_opa(lbl, rooms[i].grouped ? LV_OPA_COVER : LV_OPA_60, 0);
        lv_label_set_text(lbl, rooms[i].name);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, lx, 0);

        // Selection circle on EVERY row, so the column reads at a glance.
        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, (int)(24 * s), (int)(24 * s));
        lv_obj_align(dot, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);   // the ROW takes the tap
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 2, 0);
        lv_obj_set_style_border_color(dot, lv_color_hex(C_TEXT), 0);
        lv_obj_set_style_border_opa(dot, rooms[i].grouped ? LV_OPA_COVER : LV_OPA_40, 0);
        if (rooms[i].grouped) {
            lv_obj_set_style_bg_color(dot, lv_color_hex(C_TEXT), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        }
    }
}

// A favorite tile was tapped: ask the driver to play it (it resolves the play from the
// tile's kind — the device stays Control4-neutral and just sends the opaque id). Then
// drop back to home. Guard a stray tap on an empty-id tile.
static void onFavRow(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_nFavs || !s_favs[i].id[0]) return;
    net_play_favorite(s_favs[i].id);
    if (s_favPanel) lv_obj_add_flag(s_favPanel, LV_OBJ_FLAG_HIDDEN);
    showHome();
}
static void onFavHome(lv_event_t *e) { (void)e; goHomeFrom(s_favPanel); }

// (Re)build the favorites grid: a wrapping row of glass tiles, each showing the
// tile's artwork (driver `image` -> art_url) above its title. Artwork is fetched and
// decoded by art.c's thumbnail queue -- serviced by the same background task as the
// album art, one at a time, so it never races the now-playing decode. Tiles with no
// usable image just keep the title centred. `kind:"stream"` tiles get a small badge.
// Tapping a tile plays it (onFavRow). Called under the LVGL lock from ui_set_favorites.
// Favourite rows on the landing page. The tap plays that favourite (the driver turns
// it into an exact Play Item where the service supports it -- see PROTOCOL.md).
#define HOME_MAX_FAV 12      // hard cap on favourite rows (the stack scrolls)
#define HOME_VISIBLE_CARDS 5  // rows visible before the stack scrolls
#define FAV_REFRESH_SEC 600   // re-fetch favourites every 10 min (tick is 1 Hz)
static void onHomeFav(lv_event_t *e)
{
    lv_obj_t *t = lv_event_get_target(e);
    int i = t ? (int)(intptr_t)lv_obj_get_user_data(t) : -1;
    if (i < 0 || i >= s_nFavs || !s_favs[i].id[0]) return;
    net_play_favorite(s_favs[i].id);
}

// Replace a favourite row's generic media glyph with its real artwork. The thumbnail
// is fetched/decoded by art.c's queue and lands asynchronously; until then (or if the
// favourite has no image) the glyph stays, so a row never renders empty.
// `ix`/`isz` are the icon slot's offset and size AS THE CALLER LAID IT OUT. They
// used to be recomputed here from their own constants (22*s / 40*s), which were
// the old full-width ROW's numbers; once favourites became tiles (16*s / 38*s)
// the artwork landed offset and oversized against the slot it replaces, spilling
// over its neighbours. Never re-derive geometry a caller already knows.
static void favArt(lv_obj_t *card, lv_obj_t *icon, int i, int ix, int isz)
{
    if (!card || i < 0 || i >= s_nFavs || !s_favs[i].art_url[0]) return;
    if (isz <= 0) return;
    lv_obj_t *cv = art_thumb_add(card, ix, 0, isz, isz, s_favs[i].art_url);
    if (!cv) return;                       // pool full / no PSRAM -> keep the glyph
    lv_obj_align(cv, LV_ALIGN_LEFT_MID, ix, 0);
    if (icon) lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
}

static void rebuildFavGrid(void)
{
    if (!s_favGrid) return;
    art_thumb_clear();          // release the previous grid's thumbnails first
    lv_obj_clean(s_favGrid);
    const float s = s_uiscale;
    const bool small = (s < 0.99f);
    // Two columns everywhere: tiles are wide enough for a wrapped title on the narrow
    // 2.8" glass, and comfortable on the big X4 panels.
    const int gap = (int)(14 * s);
    const int gw  = lv_obj_get_content_width(s_favGrid);
    const int tw  = (gw - gap) / 2;
    const int th  = small ? 84 : (int)(112 * s);
    for (int i = 0; i < s_nFavs; i++) {
        lv_obj_t *tile = lv_button_create(s_favGrid);
        lv_obj_remove_style_all(tile);
        glassify(tile, (int)(16 * s));
        lv_obj_set_size(tile, tw, th);
        lv_obj_set_style_bg_opa(tile, LV_OPA_40, LV_STATE_PRESSED);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(tile, onFavRow, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        // Artwork square, centred at the top of the tile; the title sits under it.
        // Sized so art + one line of text fit the tile without reflowing it.
        int artsz = 0;
        if (s_favs[i].art_url[0]) {
            artsz = th - (small ? 34 : (int)(38 * s));
            int maxw = tw - (int)(24 * s);
            if (artsz > maxw) artsz = maxw;
            if (artsz < 24) artsz = 0;
        }
        if (artsz > 0) {
            if (!art_thumb_add(tile, (tw - artsz) / 2, (int)(8 * s), artsz, artsz,
                               s_favs[i].art_url))
                artsz = 0;      // pool full / alloc failed -> fall back to a text tile
        }

        lv_obj_t *lbl = lv_label_create(tile);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, tw - (int)(20 * s));
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(lbl, (small || artsz > 0) ? F16 : F24, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), 0);
        lv_label_set_text(lbl, s_favs[i].title);
        if (artsz > 0) lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -(int)(6 * s));
        else           lv_obj_center(lbl);

        // No kind badge. There WAS a green dot on kind:"stream" tiles, but it told
        // the user nothing: in a media-service grid every tile is a stream, so it was
        // a constant decoration overlapping the artwork. (It also rendered as a green
        // SQUARE -- it used U+25CF, which is not in any generated font range, so LVGL
        // drew its missing-glyph box.) The stream/broadcast split still matters to the
        // DRIVER, which is where it is acted on; it does not need to be on screen.
    }
    if (s_nFavs == 0) {
        lv_obj_t *l = lv_label_create(s_favGrid);
        lv_obj_set_style_text_font(l, F16, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0x9AA5B1), 0);
        lv_label_set_text(l, "No favorites");
        lv_obj_set_style_pad_top(l, (int)(20 * s), 0);
    }
}

// Room navigator favorites from the driver (`favorites` reply to getfavorites). Cache
// the tiles (a tap needs the id) and rebuild the grid. Called under the LVGL lock.
void ui_set_favorites(const favorite_t *favs, int n)
{
    if (n < 0) n = 0;
    if (n > NET_MAX_FAVORITES) n = NET_MAX_FAVORITES;
    // Compare CONTENT, not bytes. favorite_t is char arrays filled by get_str(), so
    // everything past each string's NUL is uninitialised -- a memcmp of the structs
    // reports "changed" on identical data, which here meant: rebuild -> build_home
    // re-requests the list -> reply -> "changed" -> rebuild ... an endless UI rebuild
    // loop that looked like a boot loop on the panel.
    // What counts as "changed" deliberately EXCLUDES art_url. The driver re-sends
    // this list every few minutes and the artwork URL is not stable across those
    // sends, so including it meant a full page rebuild on every refresh -- which
    // re-queued every thumbnail decode, and any rebuild landing during those couple
    // of seconds threw the results away. That is why artwork appeared on boot and
    // then vanished once the panel had been sitting: the tiles were being rebuilt
    // out from under their own decodes, forever, on a timer.
    //
    // The same favourite keeping its already-decoded cover is right anyway: id,
    // title, kind or the count changing is a real change; a re-signed URL for the
    // same artwork is not.
    bool changed = (n != s_nFavs);
    for (int i = 0; !changed && i < n; i++) {
        if (strcmp(s_favs[i].id, favs[i].id) != 0 ||
            strcmp(s_favs[i].title, favs[i].title) != 0 ||
            strcmp(s_favs[i].kind, favs[i].kind) != 0) changed = true;
    }
    if (n > 0) memcpy(s_favs, favs, (size_t)n * sizeof(favorite_t));
    s_nFavs = n;
    rebuildFavGrid();
    // The landing page renders favourites as rows, so a changed list means rebuilding
    // it. Guarded on an actual change: the driver re-sends this list on every sync.
    if (changed && s_home) ui_request_rebuild();
}

void ui_set_endpoints(const intercom_target_t *eps, int n)
{
    int cap = (int)(sizeof(s_eps) / sizeof(s_eps[0]));
    if (n < 0) n = 0;
    if (n > cap) n = cap;
    if (n > 0) memcpy(s_eps, eps, (size_t)n * sizeof(intercom_target_t));
    s_nEps = n;
    rebuildIcList();
    if (s_icToggle) {
        if (n > 0) lv_obj_clear_flag(s_icToggle, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_add_flag(s_icToggle, LV_OBJ_FLAG_HIDDEN);
    }
    // Intercom availability changed -> rebuild so the home card row adds/removes the
    // Intercom card and re-spaces the rest.
    if (s_home && ic_available() != s_homeIcShown) ui_request_rebuild();
}

// ── #11 on-device Settings overlay (display settings; Wi-Fi join is a follow-up) ─────
static lv_obj_t *s_settings;   // the overlay, NULL when closed
typedef struct { const char *const *opts; uint8_t n; uint8_t *field; uint8_t apply; lv_obj_t *val; } cyclerow_t;
static cyclerow_t s_cyc[6];    // apply: 1=backlight  2=orientation+rebuild  3=rebuild  (0=save only)
static int s_ncyc;
#ifdef MMK_CAN_ROTATE
static const char *const OPT_ORIENT[] = { "Landscape", "Portrait", "Landscape flipped", "Portrait flipped" };
#endif
static const char *const OPT_PLATFORM[] = { "Control4", "Home Assistant" };
// Idle brightness: the cycler stores an INDEX, but dim_brightness is the actual
// backlight %, so map index -> value in onCycle (apply==4). s_idleIdx mirrors it.
static const char *const OPT_IDLE[]     = { "Screen off", "5%", "10%", "25%", "50%" };
static const uint8_t      IDLE_VALS[]   = { 0, 5, 10, 25, 50 };
static uint8_t            s_idleIdx;
static const char *const OPT_MUTE[]     = { "Off", "On" };   // maps directly to g_settings.muted

// Diagnostics card: mic test status label + its auto-revert timer, so a settings
// close mid-test doesn't leave the timer firing into a freed label (LVGL deletes
// s_settings's children, including the label, on close -- the timer is a separate
// top-level object and must be torn down explicitly).
static lv_timer_t *s_micTestTimer;
static lv_obj_t   *s_micTestLbl;
// "Check for update" label auto-revert. device_ota_check_now() is fire-and-forget
// (no result callback), so without this the button label sticks on "Checking…"
// forever when we're already current. Same teardown discipline as the mic test.
static lv_timer_t *s_fwCheckTimer;
static lv_obj_t   *s_fwCheckLbl;

static void onSettingsClose(lv_event_t *e) {
    (void)e;
    if (s_micTestTimer) { lv_timer_delete(s_micTestTimer); s_micTestTimer = NULL; }
    s_micTestLbl = NULL;
    if (s_fwCheckTimer) { lv_timer_delete(s_fwCheckTimer); s_fwCheckTimer = NULL; }
    s_fwCheckLbl = NULL;
    if (s_settings) { lv_obj_del(s_settings); s_settings = NULL; }
}
// Defined later in this file; the firmware overlay below reuses them.
static lv_obj_t *settings_card(lv_obj_t *parent, const char *title);
static void settings_info_row(lv_obj_t *card, const char *label, const char *value, uint32_t valColor);

// ── Firmware update overlay: list this SKU's images from GitHub Releases and
// let the user pick one on-screen (fwupdate.c does the fetch + apply; no cloud).
static lv_obj_t  *s_fwu;        // the overlay, NULL when closed
static lv_obj_t  *s_fwuCard;    // dynamic status/list card
static lv_timer_t *s_fwuTimer;  // polls fwupdate_state()
static int        s_fwuShown;   // last state we rendered (-1 = none)
static int        s_fwuArmed;   // version index armed for a confirm tap (-1 = none)

static void onFwuClose(lv_event_t *e) {
    (void)e;
    if (s_fwuTimer) { lv_timer_delete(s_fwuTimer); s_fwuTimer = NULL; }
    if (s_fwu) { lv_obj_del(s_fwu); s_fwu = NULL; }
    s_fwuCard = NULL;
}

// A plain full-width label inside the status card.
static lv_obj_t *fwu_note(const char *text, uint32_t color) {
    lv_obj_t *l = lv_label_create(s_fwuCard);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(l, F16, 0);
    return l;
}

static void onFwuPick(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const fwupdate_rel_t *r = fwupdate_get(idx);
    if (!r || r->current) return;               // installed version isn't installable
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *lbl = btn ? lv_obj_get_child(btn, 0) : NULL;
    if (s_fwuArmed == idx) {                     // second tap: go
        if (lbl) lv_label_set_text(lbl, "Installing\xE2\x80\xA6");
        fwupdate_apply(idx);                     // -> FWU_APPLYING (poll renders it)
        return;
    }
    s_fwuArmed = idx;                            // first tap: arm + confirm
    if (lbl) lv_label_set_text(lbl, "Tap again to install");
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xC85050), 0);
}

static void fwu_render(fwupdate_state_t st) {
    if (!s_fwuCard) return;
    lv_obj_clean(s_fwuCard);
    s_fwuArmed = -1;
    if (st == FWU_FETCHING) {
        fwu_note("Checking GitHub for updates\xE2\x80\xA6", C_SUBTLE);
    } else if (st == FWU_ERROR) {
        fwu_note(fwupdate_error()[0] ? fwupdate_error() : "Update check failed", 0xC85050);
    } else if (st == FWU_APPLYING) {
        fwu_note("Downloading and installing\xE2\x80\xA6\nThe panel will restart when done.", C_TEXT);
    } else if (st == FWU_READY) {
        int n = fwupdate_count();
        for (int i = 0; i < n; i++) {
            const fwupdate_rel_t *r = fwupdate_get(i);
            if (!r) continue;
            lv_obj_t *b = lv_button_create(s_fwuCard);
            lv_obj_set_width(b, LV_PCT(100));
            lv_obj_set_style_bg_color(b, lv_color_hex(r->current ? 0x203040 : C_BTN), 0);
            lv_obj_set_style_pad_ver(b, 10, 0);
            lv_obj_add_event_cb(b, onFwuPick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_t *l = lv_label_create(b);
            char txt[80];
            if (r->current)
                snprintf(txt, sizeof(txt), "%s  \xC2\xB7 installed", r->version);
            else if (r->size > 0)
                snprintf(txt, sizeof(txt), "%s  \xC2\xB7 %.1f MB", r->version, r->size / 1048576.0);
            else
                snprintf(txt, sizeof(txt), "%s", r->version);
            lv_label_set_text(l, txt);
            lv_obj_center(l);
            lv_obj_set_style_text_font(l, F16, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(r->current ? C_GREEN : C_TEXT), 0);
        }
    }
}

static void onFwuPoll(lv_timer_t *t) {
    (void)t;
    fwupdate_state_t st = fwupdate_state();
    if ((int)st == s_fwuShown) return;   // only redraw on a state change
    s_fwuShown = (int)st;
    fwu_render(st);
}

// "Check for update" — opens the on-screen firmware picker (GitHub Releases).
static void onCheckFirmware(lv_event_t *e) {
    (void)e;
    if (s_fwu) return;
    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    s_fwu = ov;
    lv_obj_set_size(ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ov, lv_color_hex(HOME_BG_TOP), 0);
    lv_obj_set_style_bg_grad_color(ov, lv_color_hex(HOME_BG_BOT), 0);
    lv_obj_set_style_bg_grad_dir(ov, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    int pad = (int)(18 * s_uiscale);
    lv_obj_set_style_pad_all(ov, pad, 0);
    lv_obj_set_style_pad_row(ov, (int)(14 * s_uiscale), 0);
    lv_obj_set_flex_flow(ov, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ov, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(ov, LV_DIR_VER);

    // Header: back + title (same pattern as Settings).
    lv_obj_t *hdr = lv_obj_create(ov);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_width(hdr, LV_PCT(100));
    lv_obj_set_height(hdr, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, (int)(10 * s_uiscale), 0);
    {
        int bsz = (s_uiscale < 0.99f) ? 30 : (int)(30 * s_uiscale);
        lv_obj_t *back = iconBtnImg(hdr, ICON_BACK, bsz, C_BTN, LV_OPA_COVER, 0xFFFFFF,
                                    onFwuClose, NULL);
        if (back) lv_obj_set_ext_click_area(back, (int)(12 * s_uiscale));
    }
    lv_obj_t *ttl = lv_label_create(hdr);
    lv_label_set_text(ttl, "Firmware update");
    lv_obj_set_style_text_color(ttl, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(ttl, F24, 0);

    lv_obj_t *card = settings_card(ov, "Installed");
    settings_info_row(card, "Version", fw_version(), C_TEXT);

    s_fwuCard = settings_card(ov, "Available");
    s_fwuShown = -1;
    s_fwuArmed = -1;
    fwupdate_start_fetch();
    s_fwuTimer = lv_timer_create(onFwuPoll, 500, NULL);
}

// "Play test tone" — instant one-tap speaker check (audio_play_chime() is a
// short blocking call, ~0.6s, and already safe as a no-op before audio_ready()).
static void onPlayTestTone(lv_event_t *e) { (void)e; audio_play_chime(); }

// "Test mic" -- fires the shared audio_selftest_async() (chime, then a few
// seconds of live mic->speaker loopback: talk and hear yourself) and swaps the
// button label to a hint for roughly as long as the test runs, then reverts.
// Purely cosmetic timing -- the test itself runs fire-and-forget on its own
// task/thread and doesn't report back, matching how it already works over the
// network "audiotest" protocol message.
static void onMicTestDone(lv_timer_t *t) {
    (void)t;
    s_micTestTimer = NULL;
    if (s_micTestLbl) lv_label_set_text(s_micTestLbl, "Test mic");
}
static void onMicTest(lv_event_t *e) {
    if (s_micTestTimer) return;   // already running
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *lbl = btn ? lv_obj_get_child(btn, 0) : NULL;
    if (lbl) lv_label_set_text(lbl, "Talk now\xE2\x80\xA6");   // "Talk now…"
    s_micTestLbl = lbl;
    audio_selftest_async();
    // Covers the ESP-IDF path's worst case (chime + 1s tone + 2s capture + 5s
    // loopback ~= 9s); the T3 path finishes sooner and just reverts early.
    s_micTestTimer = lv_timer_create(onMicTestDone, 9500, NULL);
    lv_timer_set_repeat_count(s_micTestTimer, 1);
}

static void onCycle(lv_event_t *e) {
    cyclerow_t *c = (cyclerow_t *)lv_event_get_user_data(e);
    if (!c || !c->field) return;
    *c->field = (uint8_t)((*c->field + 1) % c->n);
    lv_label_set_text(c->val, c->opts[*c->field]);
    settings_save();
    if      (c->apply == 1) bsp_set_backlight(g_settings.brightness);
    else if (c->apply == 2) { bsp_apply_orientation(g_settings.orientation); ui_request_rebuild(); }
    else if (c->apply == 3) ui_request_rebuild();
    else if (c->apply == 4) { g_settings.dim_brightness = IDLE_VALS[*c->field]; settings_save(); } // idle % from index
    else if (c->apply == 5) {   // mute toggle -- gates ringer AND master output
        uint8_t v = g_settings.muted ? 0 : g_settings.ringer_volume;
        audio_set_ringer_volume(v);
        audio_set_user_volume(v);
    }
}

static void onBrightness(lv_event_t *e) {
    uint8_t v = (uint8_t)lv_slider_get_value(lv_event_get_target(e));
    g_settings.brightness = v;
    bsp_set_backlight(v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) settings_save();
}

static void onRingerVol(lv_event_t *e) {
    uint8_t v = (uint8_t)lv_slider_get_value(lv_event_get_target(e));
    g_settings.ringer_volume = v;
    // "Sound > Volume" is the panel's MASTER level, so drive the codec output too --
    // not just the chime amplitude. Previously this moved only the ringer, so the
    // slider appeared to do nothing for tones/diagnostics/loopback, which instead
    // followed whatever level the intercom proxy last pushed over sipvol.
    if (!g_settings.muted) { audio_set_ringer_volume(v); audio_set_user_volume(v); }
    else                   { audio_set_user_volume(0); }
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) settings_save();
}

// "Name  [value]" row — tapping [value] cycles the setting, saves, applies live.
static void settingsCycler(lv_obj_t *parent, const char *name,
                           const char *const *opts, uint8_t n, uint8_t *field, uint8_t apply) {
    if (s_ncyc >= (int)(sizeof(s_cyc) / sizeof(s_cyc[0]))) return;
    if (*field >= n) *field = 0;
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, name);
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(lbl, F16, 0);
    lv_obj_t *btn = lv_button_create(row);
    lv_obj_set_style_bg_color(btn, lv_color_hex(C_BTN), 0);
    lv_obj_set_style_pad_hor(btn, 14, 0);
    lv_obj_set_style_pad_ver(btn, 8, 0);
    lv_obj_t *val = lv_label_create(btn);
    lv_label_set_text(val, opts[*field]);
    lv_obj_set_style_text_color(val, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_text_font(val, F16, 0);
    cyclerow_t *c = &s_cyc[s_ncyc++];
    c->opts = opts; c->n = n; c->field = field; c->apply = apply; c->val = val;
    lv_obj_add_event_cb(btn, onCycle, LV_EVENT_CLICKED, c);
}

static void settings_add_logo(lv_obj_t *parent);   // NuVoxel footer (defined by the setup section)

// X4 glass card: rounded dark-translucent panel with an optional section title,
// laid out as a flex column to drop rows into.
static lv_obj_t *settings_card(lv_obj_t *parent, const char *title) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_40, 0);
    lv_obj_set_style_radius(card, (int)(16 * s_uiscale), 0);
    lv_obj_set_style_pad_all(card, (int)(16 * s_uiscale), 0);
    lv_obj_set_style_pad_row(card, (int)(9 * s_uiscale), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    if (title) {
        lv_obj_t *h = lv_label_create(card);
        lv_label_set_text(h, title);
        // White section header (X4 style); the SUBTLE grey row labels below keep the
        // hierarchy readable without the green.
        lv_obj_set_style_text_color(h, lv_color_hex(C_TEXT), 0);
        lv_obj_set_style_text_font(h, F16, 0);
    }
    return card;
}


static void settings_info_row(lv_obj_t *card, const char *label, const char *value, uint32_t valColor) {
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, lv_color_hex(C_SUBTLE), 0);
    lv_obj_set_style_text_font(l, F16, 0);
    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, value);
    lv_obj_set_style_text_color(v, lv_color_hex(valColor), 0);
    lv_obj_set_style_text_font(v, F16, 0);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
}

static void ui_show_settings(void) {
    if (s_settings) return;
    s_ncyc = 0;
    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    s_settings = ov;
    lv_obj_set_size(ov, LV_PCT(100), LV_PCT(100));
    // X4 indigo→blue gradient (same as the home screen) so the dark glass cards pop.
    lv_obj_set_style_bg_color(ov, lv_color_hex(HOME_BG_TOP), 0);
    lv_obj_set_style_bg_grad_color(ov, lv_color_hex(HOME_BG_BOT), 0);
    lv_obj_set_style_bg_grad_dir(ov, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    int pad = (int)(18 * s_uiscale);
    lv_obj_set_style_pad_all(ov, pad, 0);
    lv_obj_set_style_pad_row(ov, (int)(14 * s_uiscale), 0);
    lv_obj_set_flex_flow(ov, LV_FLEX_FLOW_COLUMN);
    // Center the card column + cap its width so it looks intentional on wide panels.
    lv_obj_set_flex_align(ov, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // A settings list scrolls vertically only. Without this any child that ends up
    // wider than the content area (as the logo used to be) adds a horizontal
    // scrollbar instead of being caught in layout.
    lv_obj_set_scroll_dir(ov, LV_DIR_VER);

    settings_add_logo(ov);   // NuVoxel wordmark banner, above the content

    // Header: BACK button + title, the same pattern every other full-screen page uses
    // (rooms, keypad, intercom, favourites). This was a small close X in the opposite
    // corner -- inconsistent with the rest of the UI and a poor touch target: at
    // 28*uiscale it lands around 28-34px, well under a comfortable finger. Reusing the
    // shared pattern fixes both, and "back" is where the user already looks.
    lv_obj_t *hdr = lv_obj_create(ov);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_width(hdr, LV_PCT(100));
    lv_obj_set_height(hdr, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, (int)(10 * s_uiscale), 0);
    {
        int bsz = (s_uiscale < 0.99f) ? 30 : (int)(30 * s_uiscale);
        lv_obj_t *back = iconBtnImg(hdr, ICON_BACK, bsz, C_BTN, LV_OPA_COVER, 0xFFFFFF,
                                    onSettingsClose, NULL);
        // Touch target beyond the drawn circle -- cheap insurance on the smaller panels.
        if (back) lv_obj_set_ext_click_area(back, (int)(12 * s_uiscale));
    }
    lv_obj_t *t = lv_label_create(hdr);
    lv_label_set_text(t, "Settings");
    lv_obj_set_style_text_color(t, lv_color_hex(C_TEXT), 0);   // X4 titles are white, not green
    lv_obj_set_style_text_font(t, F24, 0);

    // ── Device & Connection card (X4 glass) — what you can't see on-device ──────
    {
        char ip[24]; net_get_ip(ip, sizeof(ip));
        bool up = net_connected();
        bool isC4 = (g_settings.theme == 0);
        lv_obj_t *card = settings_card(ov, "Device & Connection");
        // Control4-specific fields (link status, Room, Director IP, driver protocol
        // version) only mean something when paired with a Control4 driver — the HA
        // theme has no Director to show, so this card is just the generic device
        // identity (IP/firmware/device) there.
        if (isC4) {
            settings_info_row(card, "Control4", up ? "Connected" : "Offline",
                              up ? C_GREEN : 0xC85050);
            if (up && s_lastState.room[0]) settings_info_row(card, "Room", s_lastState.room, C_TEXT);
            if (up && net_peer_ip()[0])    settings_info_row(card, "Director", net_peer_ip(), C_TEXT);
            // Just "v1" (the wire-protocol version). Only if a paired driver
            // reports a DIFFERENT proto do we surface the mismatch — hiding it
            // would defeat the point of having a protocol version at all.
            char proto[40];
            int dp = net_driver_proto();
            if (up && dp && dp != NET_PROTO_VERSION)
                snprintf(proto, sizeof(proto), "v%d (driver v%d)", NET_PROTO_VERSION, dp);
            else
                snprintf(proto, sizeof(proto), "v%d", NET_PROTO_VERSION);
            // Protocol version and driver version deliberately NOT shown: both are
            // driver-side facts, visible in Composer and the portal, and they were
            // noise on a panel someone walks up to.
        }
        settings_info_row(card, "IP address", ip[0] ? ip : "offline", ip[0] ? C_TEXT : C_SUBTLE);
        settings_info_row(card, "Firmware",   fw_version(), C_TEXT);
        {   // Check-for-update action button, right under the running version.
            lv_obj_t *fwbtn = lv_button_create(card);
            lv_obj_set_width(fwbtn, LV_PCT(100));
            lv_obj_set_style_bg_color(fwbtn, lv_color_hex(C_BTN), 0);
            lv_obj_set_style_pad_ver(fwbtn, 10, 0);
            lv_obj_add_event_cb(fwbtn, onCheckFirmware, LV_EVENT_CLICKED, NULL);
            lv_obj_t *fwlbl = lv_label_create(fwbtn);
            lv_label_set_text(fwlbl, "Check for update");
            lv_obj_center(fwlbl);
            lv_obj_set_style_text_font(fwlbl, F16, 0);
            lv_obj_set_style_text_color(fwlbl, lv_color_hex(C_TEXT), 0);
        }
        // Model / Device ID / MAC / link type / power source are all shown by the
        // Control4 driver, so they are not repeated here. What stays is what you need
        // while
        // STANDING AT the panel: is it talking to Control4, which room, what address,
        // what firmware (and can I update it), and is it licensed.
        settingsCycler(card, "Platform", OPT_PLATFORM, 2, &g_settings.theme, 3);
    }

    // ── Display card (X4 glass): brightness + the appearance options that apply ──
    {
        lv_obj_t *card = settings_card(ov, "Display");
        lv_obj_t *row = lv_obj_create(card);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Brightness");
        lv_obj_set_style_text_color(lbl, lv_color_hex(C_SUBTLE), 0);
        lv_obj_set_style_text_font(lbl, F16, 0);
        lv_obj_t *sl = lv_slider_create(row);
        lv_obj_set_width(sl, LV_PCT(50));
        lv_slider_set_range(sl, 10, 100);
        lv_slider_set_value(sl, g_settings.brightness ? g_settings.brightness : 80, LV_ANIM_OFF);
        lv_obj_add_event_cb(sl, onBrightness, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(sl, onBrightness, LV_EVENT_RELEASED, NULL);

        // Idle brightness: backlight after the screensaver timeout. "Screen off" =
        // dark, a touch wakes it. Sync the cycler index to the stored %.
        s_idleIdx = 0;
        for (uint8_t i = 0; i < 5; i++) if (IDLE_VALS[i] == g_settings.dim_brightness) { s_idleIdx = i; break; }
        settingsCycler(card, "Idle brightness", OPT_IDLE, 5, &s_idleIdx, 4);

#ifdef MMK_CAN_ROTATE
        // Orientation only where the panel can actually rotate in software (S3, ws43).
        // The fixed DSI panel (nano) + landscape-only T3 hide it.
        settingsCycler(card, "Orientation",  OPT_ORIENT, 4, &g_settings.orientation, 2);
#endif
        // Walk-up mode: keypad as the resting view (Listen/Intercom become tiles).
    }

    // ── Sound card: ringer/announcement/chime volume + mute for the panel's own
    // speaker (distinct from the media volume on the now-playing card). ──
    {
        lv_obj_t *card = settings_card(ov, "Sound");
        lv_obj_t *row = lv_obj_create(card);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Volume");
        lv_obj_set_style_text_color(lbl, lv_color_hex(C_SUBTLE), 0);
        lv_obj_set_style_text_font(lbl, F16, 0);
        lv_obj_t *sl = lv_slider_create(row);
        lv_obj_set_width(sl, LV_PCT(50));
        lv_slider_set_range(sl, 0, 100);
        lv_slider_set_value(sl, g_settings.ringer_volume, LV_ANIM_OFF);
        lv_obj_add_event_cb(sl, onRingerVol, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(sl, onRingerVol, LV_EVENT_RELEASED, NULL);
        settingsCycler(card, "Mute", OPT_MUTE, 2, &g_settings.muted, 5);
    }

    // ── Diagnostics card (X4 glass): on-device speaker + mic checks. Both
    // buttons are safe no-ops on hardware without audio (audio_play_chime() /
    // audio_selftest_async() already guard on audio_ready()). No video/camera
    // test here -- no board in the lineup has a camera today.
    {
        lv_obj_t *card = settings_card(ov, "Diagnostics");
        {
            lv_obj_t *btn = lv_button_create(card);
            lv_obj_set_width(btn, LV_PCT(100));
            lv_obj_set_style_bg_color(btn, lv_color_hex(C_BTN), 0);
            lv_obj_set_style_pad_ver(btn, 10, 0);
            lv_obj_add_event_cb(btn, onPlayTestTone, LV_EVENT_CLICKED, NULL);
            lv_obj_t *l = lv_label_create(btn);
            lv_label_set_text(l, "Play test tone");
            lv_obj_center(l);
            lv_obj_set_style_text_font(l, F16, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
        }
        {
            lv_obj_t *btn = lv_button_create(card);
            lv_obj_set_width(btn, LV_PCT(100));
            lv_obj_set_style_bg_color(btn, lv_color_hex(C_BTN), 0);
            lv_obj_set_style_pad_ver(btn, 10, 0);
            lv_obj_add_event_cb(btn, onMicTest, LV_EVENT_CLICKED, NULL);
            lv_obj_t *l = lv_label_create(btn);
            lv_label_set_text(l, "Test mic");
            lv_obj_center(l);
            lv_obj_set_style_text_font(l, F16, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
        }
    }

}

static void onSettingsOpen(lv_event_t *e) {
    (void)e;
    if (s_infoPanel) lv_obj_add_flag(s_infoPanel, LV_OBJ_FLAG_HIDDEN);
    ui_show_settings();
}

// Resolution-aware font sizing. ~480px short side is the tuned baseline
// (scale 1.0 -> keep the crisp pre-generated bitmaps); larger panels scale the
// text up via lv_tiny_ttf (arbitrary sizes from the embedded Roboto). Each
// scaled font falls back to its bitmap sibling for the merged symbol glyphs
// (♪ ★ ♥ ™ ′ ″ …) that the Roboto TTF alone lacks. Icons stay on the bitmap
// Lucide set (the 34px set is missing a few glyphs, so we don't auto-swap).
// No-op when LV_USE_TINY_TTF is off, so ESP boards stay byte-identical.
#if LV_USE_TINY_TTF
#include "mmk_roboto.h"
#include "mmk_lucide.h"
static void ui_apply_font_scale(int W, int H)
{
    static int builtShort = -1;
    int shortSide = (W < H) ? W : H;
    if (shortSide == builtShort) return;   // depends only on display size
    builtShort = shortSide;

    float scale = (float)shortSide / 480.0f;
    if (scale > 3.0f) scale = 3.0f;
    s_uiscale = scale;                     // drive layout (positions/sizes) too — MAY be <1
                                           // on the 240/320-class panels so the X4 geometry
                                           // shrinks to fit; fonts still floor at the bitmaps.
    if (scale <= 1.01f) return;            // baseline / small: keep the bitmap fonts (14/16/24/32
                                           // are the readable floor; don't render smaller than that)

    static lv_font_t *fS, *fB, *fT, *fH, *fI, *fIL;
    if (fS) lv_tiny_ttf_destroy(fS);
    if (fB) lv_tiny_ttf_destroy(fB);
    if (fT) lv_tiny_ttf_destroy(fT);
    if (fH) lv_tiny_ttf_destroy(fH);
    if (fI) lv_tiny_ttf_destroy(fI);
    if (fIL) lv_tiny_ttf_destroy(fIL);
    fS = lv_tiny_ttf_create_data(mmk_roboto_regular, mmk_roboto_regular_len, (int)(14 * scale));
    fB = lv_tiny_ttf_create_data(mmk_roboto_regular, mmk_roboto_regular_len, (int)(16 * scale));
    fT = lv_tiny_ttf_create_data(mmk_roboto_medium,  mmk_roboto_medium_len,  (int)(24 * scale));
    fH = lv_tiny_ttf_create_data(mmk_roboto_medium,  mmk_roboto_medium_len,  (int)(32 * scale));
    if (fS) { fS->fallback = &mmk_text_16; g_fSmall = fS; }
    if (fB) { fB->fallback = &mmk_text_16; g_fBody  = fB; }
    if (fT) { fT->fallback = &mmk_text_24; g_fTitle = fT; }
    if (fH) { fH->fallback = &mmk_text_32; g_fHead  = fH; }
    // Icons scale from the embedded Lucide subset (full glyph set, unlike the
    // 34px bitmap which is missing some) -- so transport/keypad icons grow with
    // the panel instead of shrinking to bitmap size on a large display.
    fI  = lv_tiny_ttf_create_data(mmk_lucide, mmk_lucide_len, (int)(24 * scale));
    fIL = lv_tiny_ttf_create_data(mmk_lucide, mmk_lucide_len, (int)(34 * scale));
    if (fI)  g_fIcon  = fI;
    if (fIL) g_fIconL = fIL;
}
#else
// No TTF engine (all ESP boards): the bitmap fonts (14/16/24/32) are fixed, so we
// can't scale type UP — but we still set s_uiscale so the X4 layout geometry
// (margins/paddings/card sizes, all `* s_uiscale`) can shrink to fit the small
// 240/320-class panels. Clamped to <=1.0 so every >=480 panel (ws43, nano)
// keeps its existing s=1.0-tuned layout byte-for-byte; only the 2.8" shrinks.
static void ui_apply_font_scale(int W, int H) {
    int shortSide = (W < H) ? W : H;
    float scale = (float)shortSide / 480.0f;
    if (scale > 1.0f) scale = 1.0f;
    s_uiscale = scale;
}
#endif

// ── X4-inspired home / main screen ──────────────────────────────────────────
// A full-screen panel shown by default: room name + a few big glass cards +
// a mini-player bar. Tapping Listen / the mini-player expands into now-playing.
static void npShowRight(int which);
static void showHome(void)
{
    // There is one resting view: the room page. The old "Primary view" setting
    // could make the keypad page the resting view instead, which stopped meaning
    // anything once the buttons moved onto the room page itself.
    s_homeAtHome = true;  if (s_home) lv_obj_clear_flag(s_home, LV_OBJ_FLAG_HIDDEN);
}
static void expandListen(lv_event_t *e) { (void)e; s_homeAtHome = false; if (s_home) lv_obj_add_flag(s_home, LV_OBJ_FLAG_HIDDEN); npShowRight(0); }
// Open the keypad / intercom as a full card over home (X4): hide home, reveal the
// panel on top. Each panel has a back chevron (onKpHome/onIcHome) to return home.
static void homeIntercom(lv_event_t *e)
{
    (void)e;
    if (!s_icPanel) return;
    if (s_x4 && s_home) { s_homeAtHome = false; lv_obj_add_flag(s_home, LV_OBJ_FLAG_HIDDEN); }
    lv_obj_clear_flag(s_icPanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_icPanel);
}
// Open the intercom picker programmatically (sim/preview + any future driver
// command), the same way ui_show_rooms_panel exposes the add-rooms panel.
void ui_show_intercom_panel(void) { homeIntercom(NULL); }

static void onHomeChevron(lv_event_t *e) { (void)e; showHome(); }
static void onHomePower(lv_event_t *e)   { (void)e; net_cmd("roomoff"); }
// Add-rooms (multiroom grouping): the X4 house+ control on both the mini-player and
// the card. Opens the now-playing card (if not already) with the room list on the
// right, and asks the driver for the live room list (-> on_rooms -> ui_set_rooms).
static void onHomeAddRooms(lv_event_t *e)
{
    (void)e;
    s_homeAtHome = false;
    if (s_home) lv_obj_add_flag(s_home, LV_OBJ_FLAG_HIDDEN);   // reveal the card
    npShowRight(2);                                            // show the room list
    net_request_rooms();                                      // refresh from the driver
}
static void onHomeVol(lv_event_t *e)
{
    if (!s_homeVol) return;
    int v = lv_slider_get_value(s_homeVol);
    if (s_homeVolIcon) lv_image_set_src(s_homeVolIcon, v == 0 ? ICON_VOL_MUTE : ICON_VOL_UP);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) net_set_volume(v);
}

// Full-screen X4 sub-page header: a back "‹" button + title at top-left (like the
// X4 "‹ Comfort" pages). Used by the keypad (and intercom) full-screen pages.
static void x4PageHeader(lv_obj_t *parent, const char *title, lv_event_cb_t backCb)
{
    const float s = s_uiscale;
    lv_obj_t *back = iconBtnImg(parent, ICON_BACK, (int)(40 * s), 0, LV_OPA_TRANSP, 0xFFFFFF, backCb, NULL);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, (int)(24 * s), (int)(26 * s));
    lv_obj_t *t = lv_label_create(parent);
    lv_obj_set_style_text_font(t, F32, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(C_TEXT), 0);
    lv_label_set_text(t, title);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, (int)(84 * s), (int)(30 * s));
}


// ── Compact-bar text rotation ───────────────────────────────────────────────
// The compact bar has room for ONE text line, but title / artist / album are all
// worth seeing. Rather than drop two of them (or scroll, which is motion the eye
// keeps chasing on a wall panel), cycle through whichever are present on a slow
// timer with a fade. One line, no layout cost, everything eventually visible.
#define MINI_ROT_MS 5000
static uint8_t      s_miniRot;        // which field is showing
static lv_timer_t  *s_miniRotTimer;
int g_ui_rot_preview = -1;            // sim: pin the phase for a static render

// Collect the non-empty fields in display order. Returns how many there are.
static int miniRotFields(const char *out[3])
{
    int n = 0;
    if (s_lastState.title[0])  out[n++] = s_lastState.title;
    if (s_lastState.artist[0]) out[n++] = s_lastState.artist;
    if (s_lastState.album[0])  out[n++] = s_lastState.album;
    // Nothing but a source name (a station with no metadata) still needs a label.
    if (n == 0 && s_lastState.source.name[0]) out[n++] = s_lastState.source.name;
    return n;
}

static void miniRotApply(bool fade)
{
    if (!s_homeMiniTitle) return;
    const char *f[3];
    int n = miniRotFields(f);
    if (n == 0) return;
    int idx = (g_ui_rot_preview >= 0) ? g_ui_rot_preview : s_miniRot;
    if (idx >= n) idx = 0;
    char buf[96];
    sanitize(f[idx], buf, sizeof(buf));
    lv_label_set_text(s_homeMiniTitle, buf);
    // The artist/album phases are secondary information -- render them a step
    // down so a glance still reads "the big one is the track".
    lv_obj_set_style_text_font(s_homeMiniTitle, idx == 0 ? F24 : F16, 0);
    lv_obj_set_style_text_color(s_homeMiniTitle,
                                lv_color_hex(idx == 0 ? C_TEXT : C_SUBTLE), 0);
    if (fade) lv_obj_fade_in(s_homeMiniTitle, 300, 0);
}

static void miniRotTick(lv_timer_t *t)
{
    (void)t;
    const char *f[3];
    int n = miniRotFields(f);
    if (n < 2) { s_miniRot = 0; return; }   // nothing to rotate through
    s_miniRot = (uint8_t)((s_miniRot + 1) % n);
    miniRotApply(true);
}



// One room-page tile, modelled on the Control4 app's room grid: a DARK tile on
// the gradient, line icon flush left, label right. Deliberately NOT Apple Home's
// "bright fill when on" -- this is wall glass in a dark room, and a white tile
// is a lamp. State is carried the way C4 carries it: the icon takes the accent
// colour and a small line above the name says what it is.
//
// `sub` is the SMALL line ABOVE the name -- the provider on a favourite ("Apple Music"),
// "On" on a lit button. One anatomy for every tile is what lets favourites,
// actions and keypad buttons share a grid without looking arbitrary.
static lv_obj_t *tileCard(lv_obj_t *p, const lv_image_dsc_t *icon, const char *glyph,
                          const char *name, const char *sub, bool on, uint32_t accent,
                          lv_event_cb_t cb, void *ud, int w, int h, lv_obj_t **iconOut,
                          int *iconX, int *iconSize)
{
    const float s = s_uiscale;
    const int pad = (int)(16 * s);
    const int isz = (int)(38 * s);
    if (iconX)   *iconX   = pad;   // published so callers overlay art on the SAME slot
    if (iconSize) *iconSize = isz;
    lv_obj_t *c = lv_button_create(p);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_radius(c, (int)(18 * s), 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(c, on ? LV_OPA_50 : LV_OPA_30, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    if (cb) lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, ud);

    // Prefer the button's OWN icon (the driver's "Button N Icon" list, drawn from
    // the Lucide font) over the generic image glyph. Without this every merged
    // button tile renders the same keypad-grid mark and the grid is unreadable at
    // a glance -- which is the entire argument for putting buttons on this page.
    lv_obj_t *ic = NULL;
    const char *g = (glyph && glyph[0]) ? iconGlyph(glyph) : NULL;
    if (g) {
        ic = lv_label_create(c);
        lv_obj_set_style_text_font(ic, FICON, 0);
        lv_label_set_text(ic, g);
        lv_obj_set_style_text_color(ic, lv_color_hex(on ? accent : C_TEXT), 0);
        lv_obj_set_width(ic, isz);
        lv_obj_set_style_text_align(ic, LV_TEXT_ALIGN_CENTER, 0);
    } else {
        ic = lv_image_create(c);
        lv_image_set_src(ic, icon);
        lv_obj_set_size(ic, isz, isz);
        lv_image_set_inner_align(ic, LV_IMAGE_ALIGN_CONTAIN);
        lv_obj_set_style_image_recolor(ic, lv_color_hex(on ? accent : C_TEXT), 0);
        lv_obj_set_style_image_recolor_opa(ic, LV_OPA_COVER, 0);
    }
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, pad, 0);
    if (iconOut) *iconOut = ic;

    const int tx = pad + isz + (int)(14 * s);
    const int tw = w - tx - pad;
    lv_obj_t *box = lv_obj_create(c);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, tw > 0 ? tw : w / 2, LV_SIZE_CONTENT);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    // lv_obj_create() is CLICKABLE by default, so this text container sat on top
    // of the tile's button and ate every tap that landed on the name -- i.e. most
    // of the tile. Only presses on the icon reached the button, which made
    // favourites, keypad buttons and intercom targets look like they worked
    // "sometimes". The label area must pass the press through to the tile.
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 0, 0);
    lv_obj_align(box, LV_ALIGN_LEFT_MID, tx, 0);

    lv_obj_t *l = lv_label_create(box);
    const lv_font_t *nf = (tw < (int)(160 * s)) ? F16 : F24;
    lv_obj_set_width(l, tw > 0 ? tw : w / 2);
    lv_obj_set_style_text_font(l, nf, 0);
    // Pin the line count: LONG_DOT wraps first and only ellipsises when it runs
    // out of HEIGHT, so a content-sized label just grows extra lines and shoves
    // the state line out of the tile ("Mike DeLuca's / Station / stream").
    // A tile with NO state line gets TWO lines instead of one -- favourites have
    // no state and long names ("Mike DeLuca's Station"), and with no browse on
    // the panel they are the only way to start music, so the name has to be
    // readable. The second line is space the tile already has.
    // Line count: a tile WITH a state line keeps the name to one line; a
    // stateless one (a favourite, an unlit button) may use as many lines as the
    // tile holds, up to three -- "Mike DeLuca's Station" needs all three at WS43
    // width, and with no browse on the panel a favourite's name is the only thing
    // identifying it.
    //
    // MEASURE the wrapped text rather than always reserving the maximum: LVGL
    // renders a short string at the TOP of an over-tall label, so a fixed 3-line
    // box left "Chill Jazz" floating above its own icon.
    const int lh = lv_font_get_line_height(nf);
    int lines = 1;
    if (!(sub && sub[0])) {
        int maxLines = (h - (int)(16 * s)) / lh;
        if (maxLines < 2) maxLines = 2;
        if (maxLines > 3) maxLines = 3;
        lv_point_t sz;
        lv_text_get_size(&sz, name, nf, 0, 0, tw > 0 ? tw : w / 2, LV_TEXT_FLAG_NONE);
        lines = (sz.y + lh - 1) / lh;
        if (lines < 1) lines = 1;
        if (lines > maxLines) lines = maxLines;
    }
    lv_obj_set_height(l, lh * lines);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
    lv_label_set_text(l, name);
    // State BELOW the name. Apple Home and Home Assistant both read
    // name-then-state; only the C4 app puts the provider above, and mixing the
    // two in one grid is what makes a tile list look arbitrary.
    if (sub && sub[0]) {
        lv_obj_t *sl = lv_label_create(box);
        lv_obj_set_width(sl, tw > 0 ? tw : w / 2);
        lv_label_set_long_mode(sl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(sl, F16, 0);
        lv_obj_set_style_text_color(sl, lv_color_hex(on ? accent : C_SUBTLE), 0);
        lv_label_set_text(sl, sub);
    }
    return c;
}


// Tap a merged keypad-button tile. The dedicated keypad page indexes GRID SLOTS
// (s_kpId, which carries synthetic Listen/Intercom sentinels); here the tiles map
// straight to s_lastState.buttons, so it needs its own handler rather than
// reusing onKpBtn with a fabricated slot index.
static void onHomeBtn(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_lastState.n_buttons || i >= MMK_MAX_BUTTONS) return;
    net_send_button(s_lastState.buttons[i].id);
}

// The room page: ONE bounded, scrolling, wrapping tile grid holding the room's
// favourites and its keypad buttons. There is no separate keypad screen -- the
// buttons ARE tiles here, which is how the Control4 app puts Exit Gate / Garage
// Door on a room page.
//
// Listen deliberately gets no tile: the now-playing bar below is the
// now-playing affordance and tapping it opens the full page, so a tile would
// just duplicate the thing directly beneath it.
static void build_home_tiles(int W, int H, bool smallP)
{
    const float s = s_uiscale;
    const int m   = (int)(40 * s);
    const int gap = (int)(14 * s);
    const bool portrait = (H > W);

    const bool icAvail = ic_available();
    s_homeIcShown = icAvail;

    int nfav = s_nFavs; if (nfav > HOME_MAX_FAV) nfav = HOME_MAX_FAV;
    int nbtn = s_haveButtons ? s_lastState.n_buttons : 0;
    if (nbtn > MMK_MAX_BUTTONS) nbtn = MMK_MAX_BUTTONS;

    const int cy = smallP ? (int)(44 * s) : (int)(104 * s);

    // Columns follow WIDTH in RAW pixels: a scaled target grows in step with s
    // and so always yields the same count, which is why a 1280px panel kept
    // producing two enormous columns. Portrait wants a smaller target -- at
    // 480px the landscape target gives one column, i.e. the full-width row list
    // this layout replaced.
    int cols = portrait ? (((W - 2 * m) + gap) / (190 + gap))
                        : (((W - 2 * m) + gap) / (340 + gap));
    if (cols < 1) cols = 1;
    if (cols > 4) cols = 4;
    if (portrait && cols > 2) cols = 2;

    const int miniH = smallP ? 0 : (int)(104 * s) + (int)(24 * s);
    int avail = H - miniH - (int)(16 * s) - cy;

    // Size the tiles to the space rather than fixing their height, then snap the
    // viewport to whole rows. A fixed tile height left ~130px dead at the bottom
    // of the 10" AND still pushed two buttons off the page -- that slack was
    // worth an entire extra row. So: work out how many rows the content needs,
    // take as many as fit (within touch limits), and stretch the tiles into the
    // remainder. It still snaps because a part-height row reads as broken rather
    // than as "there is more below".
    const int ntot  = (icAvail ? 1 : 0) + nfav + nbtn;
    const int need  = (ntot + cols - 1) / cols;
    // Prefer MORE ROWS over taller tiles: a proportional floor scaled to ~96px on
    // the 10", which rejected three 93px rows in favour of two 150px ones --
    // fatter tiles showing less. The floor is a touch-target minimum, not a
    // proportional one.
    const int chMax = (int)(90 * s), chMin = (int)(48 * s) < 56 ? 56 : (int)(48 * s);
    int ch = (int)(76 * s);
    for (int r = need; r >= 1; r--) {
        int fit = (avail - (r - 1) * gap) / r;
        if (fit >= chMin) { ch = (fit > chMax) ? chMax : fit; break; }
    }
    {
        int vrows = (avail + gap) / (ch + gap);
        if (vrows < 1) vrows = 1;
        // Spend the leftover on the tiles rather than leaving a band of empty
        // gradient under the grid.
        int grown = (avail - (vrows - 1) * gap) / vrows;
        if (grown > ch && grown <= chMax) ch = grown;
        int snapped = vrows * ch + (vrows - 1) * gap;
        if (snapped <= avail) avail = snapped;
    }

    lv_obj_t *grid = lv_obj_create(s_home);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, W - 2 * m, avail);
    lv_obj_set_pos(grid, m, cy);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, gap, 0);
    lv_obj_set_style_pad_row(grid, gap, 0);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    // More tiles than fit simply scroll; the scrollbar shows at rest so it is
    // discoverable.
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(grid, lv_color_hex(0xFFFFFF), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(grid, LV_OPA_30, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(grid, LV_OPA_70, LV_PART_SCROLLBAR | LV_STATE_SCROLLED);
    lv_obj_set_style_width(grid, (int)(5 * s), LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(grid, (int)(10 * s), 0);

    const int gw = W - 2 * m - (int)(10 * s);            // usable width inside the grid
    const int cw = (gw - (cols - 1) * gap) / cols;

    if (icAvail)
        tileCard(grid, ICON_INTERCOM, "Bell", "Intercom", NULL, false, 0x4CC9F0,
                 homeIntercom, NULL, cw, ch, NULL, NULL, NULL);

    // Favourites lead the content: with no browse on the panel they are the only
    // way to start music here.
    for (int i = 0; i < nfav; i++) {
        lv_obj_t *ficon = NULL;
        int fx = 0, fsz = 0;
        // No sub-line: the C4 app puts the PROVIDER there ("Apple Music"), which we do
        // not have -- and echoing the raw kind put "stream" under every
        // favourite, which is noise rather than information.
        lv_obj_t *c = tileCard(grid, ICON_MEDIA, NULL, s_favs[i].title, NULL,
                               false, 0x4CC9F0, onHomeFav, (void *)(intptr_t)i,
                               cw, ch, &ficon, &fx, &fsz);
        lv_obj_set_user_data(c, (void *)(intptr_t)i);
        favArt(c, ficon, i, fx, fsz);   // art lands on the icon's own slot
    }

    for (int i = 0; i < nbtn; i++) {
        const key_btn_t *b = &s_lastState.buttons[i];
        // An unnamed button is an unconfigured slot -- no tile. The driver already
        // filters these out; this also protects against an older driver that still
        // sends every slot the panel says it can display.
        if (!b->label[0]) continue;
        uint32_t accent = 0xFFD166;
        if (b->color[0]) accent = (uint32_t)strtoul(b->color, NULL, 16);
        const char *gi = (!b->on && b->off_icon[0]) ? b->off_icon : b->icon;
        tileCard(grid, ICON_KEYPAD, gi, b->label, b->on ? "On" : NULL, b->on,
                 accent, onHomeBtn, (void *)(intptr_t)i, cw, ch, NULL, NULL, NULL);
    }
}

static void build_home(lv_obj_t *scr, int W, int H, uint32_t bgTop, uint32_t bgBot)
{
    const float s = s_uiscale;
    const int m = (int)(40 * s);
    const bool portrait = (H > W);
    // Tiny 2.8" class (240x320 portrait OR 320x240 landscape — s<1 either way, it's
    // keyed off the short side): no room for the mini-player or the "Home"/room
    // heading — just a clean stack of big touch cards (per user). Card ARRANGEMENT
    // (stacked vs a row) still follows `portrait` below; only this chrome-trim flag
    // is orientation-agnostic.
    const bool smallP = (s < 0.99f);
    s_home = lv_obj_create(scr);
    lv_obj_remove_style_all(s_home);
    lv_obj_set_size(s_home, W, H);
    lv_obj_set_pos(s_home, 0, 0);
    lv_obj_set_style_bg_color(s_home, lv_color_hex(bgTop), 0);
    lv_obj_set_style_bg_grad_color(s_home, lv_color_hex(bgBot), 0);
    lv_obj_set_style_bg_grad_dir(s_home, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_home, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_home, LV_OBJ_FLAG_SCROLLABLE);

    if (!smallP) {
        lv_obj_t *room = lv_label_create(s_home);
        lv_obj_set_style_text_font(room, F32, 0);
        lv_obj_set_style_text_color(room, lv_color_hex(C_TEXT), 0);
        // The landing page is titled with the ROOM, not "Home" -- this panel belongs to
        // a room and that is the more useful label. "Home" is only the fallback for
        // before the driver has told us the room. Kept in s_homeTitle because this was
        // previously set ONCE at build time: a room arriving (or changing) afterwards
        // never updated it, so it read "Home" forever on a panel that knew its room.
        lv_label_set_text(room, s_lastState.room[0] ? s_lastState.room : "Home");
        lv_obj_set_pos(room, m, (int)(30 * s));
        s_homeTitle = room;
    } else s_homeTitle = NULL;
    // Settings: a three-dot menu in the TOP-RIGHT, level with the room title. This was
    // a gear pinned bottom-right (with a star bottom-left for favourites) -- two pieces
    // of floating chrome competing with the content. Top-right "more" is where the rest
    // of the UI already puts a menu (the now-playing card), so this is consistent, and
    // it frees the bottom of the page for real content.
    { int gsz = smallP ? 34 : (int)(40 * s);
      lv_obj_t *more = iconBtnImg(s_home, ICON_DOTS, gsz, 0x000000, LV_OPA_40,
                                  0xFFFFFF, onSettingsOpen, NULL);
      lv_obj_align(more, LV_ALIGN_TOP_RIGHT, -(int)(12 * s), (int)(28 * s));
      lv_obj_set_ext_click_area(more, (int)(12 * s)); }

    // Record intercom availability BEFORE any early-out below. ui_tick asks for a
    // rebuild whenever ic_available() disagrees with this flag, and the
    // not-connected path returns without ever building the tile grid -- which is
    // where the flag used to be set. So a panel that was licensed for intercom and
    // had lost its link requested a rebuild, rebuilt, skipped the assignment, and
    // requested another. A live panel in the field had done this 4136 times,
    // pinning its CPU so it could not answer SDDP or hold the link that would have
    // cleared the condition. The flag must be updated on EVERY path that builds
    // this page, not just the connected one.
    s_homeIcShown = ic_available();

    // NOT CONNECTED: say so, instead of offering tiles that cannot do anything.
    // Every one of those actions is a round trip to the driver, so
    // with the link down they are dead buttons that look live -- the panel appeared
    // functional and simply ignored taps. Settings stays reachable (top-right) because
    // that is exactly where someone goes to check the address/link when it is broken.
    if (!s_connected) {
        lv_obj_t *box = lv_obj_create(s_home);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, W - 2 * m, LV_SIZE_CONTENT);
        lv_obj_set_pos(box, m, (int)((smallP ? 70 : 150) * s));
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(box, (int)(10 * s), 0);

        // A muted X in a ring: unambiguously "no link". Deliberately an IMAGE icon
        // (ICON_CLOSE) rather than a font glyph -- the icon fonts are generated with
        // limited ranges, and both "Plug" and U+25CF rendered as missing-glyph boxes
        // when tried here. Image icons always render.
        int isz = (int)((smallP ? 46 : 64) * s);
        lv_obj_t *ring = lv_obj_create(box);
        lv_obj_remove_style_all(ring);
        lv_obj_set_size(ring, isz, isz);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(ring, (int)(2 * s) < 2 ? 2 : (int)(2 * s), 0);
        lv_obj_set_style_border_color(ring, lv_color_hex(C_TEXT), 0);
        lv_obj_set_style_border_opa(ring, LV_OPA_30, 0);
        {
            lv_obj_t *x = lv_image_create(ring);
            lv_image_set_src(x, ICON_CLOSE);
            int xs = (int)(isz * 0.5f);
            lv_obj_set_size(x, xs, xs);
            lv_image_set_inner_align(x, LV_IMAGE_ALIGN_CONTAIN);
            lv_obj_set_style_image_recolor(x, lv_color_hex(C_TEXT), 0);
            lv_obj_set_style_image_recolor_opa(x, LV_OPA_40, 0);
            lv_obj_center(x);
        }

        lv_obj_t *l1 = lv_label_create(box);
        lv_label_set_text(l1, "Not connected");
        lv_obj_set_style_text_color(l1, lv_color_hex(C_TEXT), 0);
        lv_obj_set_style_text_font(l1, smallP ? F16 : F24, 0);

        lv_obj_t *l2 = lv_label_create(box);
        lv_obj_set_width(l2, W - 2 * m - (int)(20 * s));
        lv_obj_set_style_text_align(l2, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(l2, LV_LABEL_LONG_WRAP);
        lv_label_set_text(l2, "Waiting for the Control4 controller");
        lv_obj_set_style_text_color(l2, lv_color_hex(C_SUBTLE), 0);
        lv_obj_set_style_text_font(l2, F16, 0);

        // The panel's own address: the first thing anyone needs when the link is down.
        char ipbuf[40] = {0};
        net_get_ip(ipbuf, sizeof(ipbuf));
        if (ipbuf[0]) {
            lv_obj_t *l3 = lv_label_create(box);
            lv_label_set_text(l3, ipbuf);
            lv_obj_set_style_text_color(l3, lv_color_hex(C_MUTED), 0);
            lv_obj_set_style_text_font(l3, F16, 0);
        }
        goto home_chrome;   // skip the action cards + mini-player
    }

    build_home_tiles(W, H, smallP);

    // Mini-player bar (bottom): a big square album-art tile on the left, title +
    // artist stacked next to it, a persistent volume slider along the lower band,
    // and a right cluster with power stacked above pause · skip. Omitted on the tiny
    // 2.8" (smallP) — no vertical room; the cards fill the screen instead — and on
    // design 3, which gives the whole page to tiles (the Listen chip carries the
    // now-playing info instead, the way the C4 room page does).
    if (!smallP) {
    // A slim bar: the tile grid is the content now, and a full-height player left
    // it a row and a half on the 10". Dropping the bar altogether would also drop
    // at-a-glance volume and transport, which is the most-used control on a wall
    // keypad -- so it stays, just smaller.
    const int bh = (int)(104 * s);
    const int by = H - bh - (int)(24 * s);
    lv_obj_t *bar = lv_button_create(s_home);
    lv_obj_remove_style_all(bar);
    glassify(bar, (int)(20 * s));
    lv_obj_set_size(bar, W - 2 * m, bh);
    lv_obj_set_pos(bar, m, by);
    lv_obj_add_event_cb(bar, expandListen, LV_EVENT_CLICKED, NULL);
    s_homeBar = bar;
    // Hidden until the room is on + something is playing (X4 shows no mini-player
    // when the room is off). setState reconciles this from live state.
    setVis(bar, s_haveState && s_lastState.power);

    const int pad = (int)(14 * s);
    const int asz = bh - 2 * pad;                 // square art fills the bar height
    lv_obj_t *art = lv_obj_create(bar);
    lv_obj_remove_style_all(art);
    lv_obj_set_size(art, asz, asz);
    // Same trap as the tile label box: clickable by default, so a tap on the
    // album art never reached the bar underneath. The bar is now the only route
    // to the full now-playing page, so the art has to pass presses through.
    lv_obj_clear_flag(art, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(art, LV_ALIGN_LEFT_MID, pad, 0);
    lv_obj_set_style_radius(art, (int)(10 * s), 0);
    lv_obj_set_style_clip_corner(art, true, 0);   // clips the art canvas to rounded corners
    lv_obj_set_style_bg_color(art, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(art, LV_OPA_40, 0);
    art_add_mirror(art, 0, 0, asz, asz);          // real album art (cover-cropped mirror)
    lv_obj_t *ai = lv_image_create(art);          // music-note placeholder while no art
    lv_image_set_src(ai, ICON_MEDIA);
    lv_obj_set_size(ai, (int)(44 * s), (int)(44 * s));
    lv_image_set_inner_align(ai, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_set_style_image_recolor(ai, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_image_recolor_opa(ai, LV_OPA_50, 0);
    lv_obj_center(ai);
    setVis(ai, !art_has());
    s_homeMiniArt = art;
    s_homeMiniNote = ai;

    const int textX = pad + asz + (int)((portrait ? 18 : 24) * s);
    // The right cluster is ONE row now (power · pause · next), so reserve its full
    // width. It used to be two rows -- power above, pause/skip below -- which the
    // slimmer bar cannot hold: at 104*s the top-aligned power (16..58) and the
    // bottom-aligned next (42..84) overlapped by 16px.
    const int rcBtn = (int)(42 * s), rcGap = (int)(10 * s);
    const int rcW   = 2 * rcBtn + rcGap + (int)(20 * s);   // power + play/pause
    // Add-rooms (house+) button leads the title row (landscape only — portrait is too
    // narrow for it; add-rooms is still reachable from the now-playing card there).
    int titleX = textX;
    if (!portrait) {
        const int arSz = (int)(40 * s);
        lv_obj_t *ar = iconBtnImg(bar, ICON_ROOM_ADD, arSz, 0, LV_OPA_TRANSP, 0xFFFFFF, onHomeAddRooms, NULL);
        lv_obj_align(ar, LV_ALIGN_TOP_LEFT, textX - (int)(6 * s), (int)(18 * s));
        titleX = textX + arSz + (int)(6 * s);
    }
    const int textW = W - 2 * m - titleX - rcW;

    // Title + artist are each capped to ONE line: LV_LABEL_LONG_DOT wraps to
    // multiple lines by default (a long title then collides with the artist), so
    // pin the height to a single line height and stack the artist below it.
    // The bar has room for ONE text line above the volume band, so it shows a
    // single rotating line (title -> artist -> album) and the artist label stays
    // hidden rather than colliding with the slider.
    const int titleY = (int)(14 * s);
    const int titleLH = lv_font_get_line_height(F24);
    s_homeMiniTitle = lv_label_create(bar);
    lv_obj_set_style_text_font(s_homeMiniTitle, F24, 0);
    lv_obj_set_style_text_color(s_homeMiniTitle, lv_color_hex(C_TEXT), 0);
    lv_obj_set_size(s_homeMiniTitle, textW, titleLH);
    lv_label_set_long_mode(s_homeMiniTitle, LV_LABEL_LONG_DOT);
    lv_obj_align(s_homeMiniTitle, LV_ALIGN_TOP_LEFT, titleX, titleY);
    lv_label_set_text(s_homeMiniTitle, "-");
    s_homeMiniArtist = lv_label_create(bar);
    lv_obj_set_style_text_font(s_homeMiniArtist, F16, 0);
    lv_obj_set_style_text_color(s_homeMiniArtist, lv_color_hex(C_SUBTLE), 0);
    lv_obj_set_size(s_homeMiniArtist, textW, lv_font_get_line_height(F16));
    lv_label_set_long_mode(s_homeMiniArtist, LV_LABEL_LONG_DOT);
    lv_obj_align(s_homeMiniArtist, LV_ALIGN_TOP_LEFT, titleX, titleY + titleLH + (int)(8 * s));
    lv_label_set_text(s_homeMiniArtist, "");
    {
        lv_obj_add_flag(s_homeMiniArtist, LV_OBJ_FLAG_HIDDEN);
        // One rotation timer per built bar; torn down with the rest of the home
        // panel so it can never fire into a freed label.
        if (s_miniRotTimer) lv_timer_delete(s_miniRotTimer);
        s_miniRot = 0;
        s_miniRotTimer = lv_timer_create(miniRotTick, MINI_ROT_MS, NULL);
    }

    // Persistent volume: speaker icon + long slider along the lower band.
    lv_obj_t *vi = lv_image_create(bar);
    lv_image_set_src(vi, ICON_VOL_UP);
    lv_obj_set_size(vi, (int)(28 * s), (int)(28 * s));
    lv_image_set_inner_align(vi, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_set_style_image_recolor(vi, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_image_recolor_opa(vi, LV_OPA_COVER, 0);
    lv_obj_align(vi, LV_ALIGN_BOTTOM_LEFT, textX, (int)(-26 * s));
    s_homeVolIcon = vi;
    s_homeVol = lv_slider_create(bar);
    lv_slider_set_range(s_homeVol, 0, 100);
    lv_obj_set_size(s_homeVol, textW - (int)(44 * s), (int)(10 * s));
    lv_obj_align(s_homeVol, LV_ALIGN_BOTTOM_LEFT, textX + (int)(44 * s), (int)(-34 * s));
    lv_obj_set_ext_click_area(s_homeVol, (int)(22 * s));
    lv_obj_set_style_bg_color(s_homeVol, lv_color_hex(0x404040), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_homeVol, lv_color_hex(C_TEXT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_homeVol, lv_color_hex(C_TEXT), LV_PART_KNOB);
    lv_obj_add_event_cb(s_homeVol, onHomeVol, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_homeVol, onHomeVol, LV_EVENT_VALUE_CHANGED, NULL);

    // Right cluster: power · play/pause, in one vertically-centred row, spaced by a
    // full gap so no two targets touch. NO skip-next: the Control4 app's mini
    // player offers play/pause only, and dropping it also buys the two remaining
    // targets more room (they were crowding the power button).
    const int rcX = (int)(14 * s);
    lv_obj_t *pp = iconBtnImg(bar, ICON_PAUSE, rcBtn, 0, LV_OPA_TRANSP, 0xFFFFFF, onPlayPause, &s_homeMiniPP);
    lv_obj_align(pp, LV_ALIGN_RIGHT_MID, -rcX, 0);
    lv_obj_t *pw = iconBtnImg(bar, ICON_POWER, rcBtn, 0, LV_OPA_TRANSP, 0xFFFFFF, onHomePower, NULL);
    lv_obj_align(pw, LV_ALIGN_RIGHT_MID, -(rcX + rcBtn + rcGap), 0);
    }  // if (!smallP) mini-player

home_chrome:
    (void)0;   // label target for the not-connected early-out above
}

#if MMK_SNAPSHOT
// ── Dev screenshot + remote tap over serial (host: tools/esp_shot.py) ────────
// A task dumps the active screen's framebuffer on a 0x02 byte (and once after
// boot), framed "<<SNAP w h stride>>\n" + raw RGB565 + "\n<<SNAPEND>>\n". It
// also injects a synthetic touch on "0x03 xh xl yh yl" (screen coords, u16 BE),
// via a private LVGL pointer indev — so the whole UI is navigable remotely
// during the X4 redesign without physically tapping the panel.
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "others/snapshot/lv_snapshot.h"
// s_injStage counts down: >1 = held PRESSED this many more reads, ==1 = the
// RELEASE read (completes the click), 0 = idle. Holding the press for a few
// poll cycles makes it register as a clean click (a 1-cycle blip can miss).
static volatile int s_injX = 0, s_injY = 0, s_injStage = 0;
static void inj_read(lv_indev_t *i, lv_indev_data_t *d)
{
    (void)i;
    d->point.x = s_injX; d->point.y = s_injY;
    if (s_injStage > 1)      { d->state = LV_INDEV_STATE_PRESSED;  s_injStage--; }
    else if (s_injStage == 1) { d->state = LV_INDEV_STATE_RELEASED; s_injStage = 0; }
    else                        d->state = LV_INDEV_STATE_RELEASED;
}
static int getc_blk(void) { int c; while ((c = getchar()) < 0) vTaskDelay(pdMS_TO_TICKS(20)); return c; }
static void snap_task(void *arg)
{
    (void)arg;
    if (lvgl_port_lock(2000)) {
        lv_indev_t *inj = lv_indev_create();
        lv_indev_set_type(inj, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(inj, inj_read);
        lvgl_port_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(3500));   // let the boot screen settle, then auto-dump once
    bool trig = true;
    for (;;) {
        int c = trig ? 0x02 : getchar();
        trig = false;
        if (c == 0x02) {
            if (lvgl_port_lock(4000)) {
                // Overlays (Settings / intercom picker) live on lv_layer_top(), which is
                // NOT part of the active screen — snapshot the top layer when it has an
                // open (full-screen, opaque) overlay so those pages are captured too.
                lv_obj_t *top = lv_layer_top();
                lv_obj_t *tgt = (lv_obj_get_child_count(top) > 0) ? top : lv_screen_active();
                lv_draw_buf_t *b = lv_snapshot_take(tgt, LV_COLOR_FORMAT_RGB565);
                if (b) {
                    printf("\n<<SNAP %d %d %d>>\n", (int)b->header.w, (int)b->header.h, (int)b->header.stride);
                    fwrite(b->data, 1, b->data_size, stdout);
                    fflush(stdout);
                    printf("\n<<SNAPEND>>\n");
                    lv_draw_buf_destroy(b);
                } else {
                    printf("<<SNAPERR take>>\n");
                }
                lvgl_port_unlock();
            }
        } else if (c == 0x03) {
            int xh = getc_blk(), xl = getc_blk(), yh = getc_blk(), yl = getc_blk();
            s_injX = (xh << 8) | xl; s_injY = (yh << 8) | yl;
            s_injStage = 5;   // hold pressed ~4 polls, then release = a clean click
            printf("<<TAP %d %d>>\n", s_injX, s_injY);
        } else if (c < 0) {
            vTaskDelay(pdMS_TO_TICKS(50));   // no console byte available — poll
        }
    }
}
void ui_snapshot_start(void) { xTaskCreate(snap_task, "snap", 8192, NULL, 3, NULL); }
#endif


// Swallows the touch that WAKES a dimmed panel. Without it the wake tap also
// lands on whatever happens to be under the finger -- walk up to a dark screen,
// tap it to see what is playing, and you have fired a keypad button. The shield
// is a transparent, clickable full-screen object on the SYSTEM layer (above
// lv_layer_top, so it also covers the settings/setup overlays); it is shown only
// while dimmed. Because the press lands on a real object, LVGL still counts it as
// activity, so the inactivity timer resets and the next tick brings the backlight
// back up -- the tap wakes, and does nothing else.
static lv_obj_t *s_wakeShield;
static void onWakeTap(lv_event_t *e)
{
    (void)e;
    if (s_wakeShield) lv_obj_add_flag(s_wakeShield, LV_OBJ_FLAG_HIDDEN);
}

void ui_begin(void)
{
    ui_apply_theme();   // pick X4 vs HA before anything below reads g_theme
    lv_obj_t *scr = lv_screen_active();
    // Created once and NOT torn down by a rebuild: it lives on the system layer,
    // which lv_obj_clean(screen) does not touch.
    if (!s_wakeShield) {
        s_wakeShield = lv_obj_create(lv_layer_sys());
        lv_obj_remove_style_all(s_wakeShield);
        lv_obj_set_size(s_wakeShield, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_opa(s_wakeShield, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(s_wakeShield, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_wakeShield, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_wakeShield, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s_wakeShield, onWakeTap, LV_EVENT_PRESSED, NULL);
    }
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 0, 0);
    // Base background — immediately overridden below to the theme's HOME_BG
    // gradient (X4 is universal now), but set here too so there's no unstyled
    // flash before that runs.
    lv_obj_set_style_bg_color(scr, lv_color_hex(HOME_BG_TOP), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(HOME_BG_BOT), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    const int W = lv_display_get_horizontal_resolution(NULL);
    const int H = lv_display_get_vertical_resolution(NULL);
    ui_apply_font_scale(W, H);   // pick resolution-appropriate font sizes first

    const bool landscape = (W >= H);
    // X4 two-column now-playing: album-art card atop stacked controls in a left
    // column (right column = queue, deferred). Enabled on large landscape panels
    // (the T3 7"). WIP first pass -- reuses the existing controls, repositioned.
    // X4 card styling comes in two resolution-driven flavors (no board #ifdef):
    //  • x4L — large-landscape TWO-column now-playing (nano 10.1" @ 1280x800): the
    //    art card sits in a left column, info/rooms panels fill the right column.
    //  • x4P — narrow PORTRAIT single column (ws43 4.3" @ 480x800): art near the top,
    //    controls stacked below it, info/rooms shown as full-card overlays. The
    //    W<=700 cap keeps this to the ~480 class and off the nano's 800-wide portrait.
    const bool x4L = landscape && W >= 1000;
    // Portrait X4: the 4.3" class (480-wide) AND the small 2.8" (240-wide) — the
    // layout scales by s_uiscale (which now goes below 1 on the small panel). The
    // <=900 cap is a safety net, not a tuned target: no board defaults to or
    // exposes portrait in its Settings UI at this width (nano is landscape-only,
    // 1280x800 -> x4L), but net.c applies a driver-pushed `rotation` unconditionally
    // (not gated by MMK_CAN_ROTATE), so nano COULD be told to go portrait
    // (800x1280) by a driver even with no on-device control for it. Rather than
    // leave that state with no X4 layout at all, it renders (unpolished) via this
    // same single-column path instead of falling back to the legacy overlay UI.
    const bool x4P = !landscape && W >= 200 && W <= 900;
    // Small landscape X4 (e.g. the 2.8" s3 rotated, 320x240): the same small-panel
    // class as x4P (same s_uiscale — it's keyed off the SHORT side, which is 240
    // either way) but landscape, so it doesn't fit x4P's `!landscape` gate. Reuses
    // x4P's single-column stacked layout via x4V below rather than x4L's big
    // two-column design (no room for two columns at 320 wide).
    const bool x4Ls = landscape && W < 1000 && H >= 200;
    const bool x4  = x4L || x4P || x4Ls;
    s_x4 = x4;
    // x4V ("vertical stack") = the single-column layout shape shared by x4P and
    // x4Ls, as opposed to x4L's big two-column design. Nearly every x4P-only
    // layout decision below is really about this SHAPE, not true portrait, so it
    // uses x4V — x4P is kept only where true orientation actually matters.
    const bool x4V = x4P || x4Ls;
    // Small X4 (either orientation, s_uiscale<1 on the 240-class panel): s_uiscale
    // shrinks all the scaled gaps below the fixed bitmap-font heights, so this
    // flavor needs font-aware spacing + a trimmed bottom (no room/chevron chrome —
    // the room is on Home and there's already a top-left back button). Doesn't
    // affect the >=480-wide boards (ws43/nano), which stay at s=1.0.
    const bool smallP = x4V && (s_uiscale < 0.99f);
    printf("ui: res %dx%d landscape=%d x4=%d (L=%d P=%d Ls=%d)\n", W, H, (int)landscape, (int)x4, (int)x4L, (int)x4P, (int)x4Ls);
    // Card inset margin (gradient shows around it). The tiny 240x320 (smallP) has no
    // room to waste on a card frame, so it goes FULL-SCREEN (no margin, no card panel).
    const int x4M = smallP ? 0 : (int)(28 * s_uiscale);
    // Content column: the left ~46% of the card in landscape; the full inset width
    // in portrait (so title/bar/transport/volume all flow full-width, single column).
    const int x4ColX = x4V ? (x4M + (int)(smallP ? 14 : 20 * s_uiscale)) : (int)(52 * s_uiscale);
    const int x4ColW = x4V ? (W - 2 * x4ColX) : (W * 46 / 100 - x4ColX);
    // X4 now-playing is an inset CARD floating on the home gradient (not full-bleed):
    // a dark rounded panel, with the art + controls + right panel drawn on top of it.
    // Created before art_begin so the art canvas and widgets render above it. smallP
    // skips the card entirely and paints the controls straight on the full-screen gradient
    // (scr already carries the HOME_BG gradient, set above).
    s_npCard = NULL;
    if (!smallP) {
        s_npCard = lv_obj_create(scr);
        lv_obj_remove_style_all(s_npCard);
        // Landscape card runs to the very bottom edge (more room for a larger art +
        // the control stack); portrait keeps a bottom margin so it reads as a card.
        lv_obj_set_size(s_npCard, W - 2 * x4M, x4V ? (H - 2 * x4M) : (H - x4M));
        lv_obj_set_pos(s_npCard, x4M, x4M);
        lv_obj_set_style_radius(s_npCard, (int)(26 * s_uiscale), 0);
        lv_obj_set_style_bg_color(s_npCard, lv_color_hex(0x0E1120), 0);
        lv_obj_set_style_bg_grad_color(s_npCard, lv_color_hex(0x121A30), 0);
        lv_obj_set_style_bg_grad_dir(s_npCard, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(s_npCard, LV_OPA_90, 0);   // gradient bleeds through slightly
        lv_obj_clear_flag(s_npCard, LV_OBJ_FLAG_SCROLLABLE);
    }

    int aX, aY, aW, aH;
    if (x4V) {
        // Portrait/small-landscape: a big near-square cover near the top, full
        // column width. On the tiny 240x320 the vertical budget is tight, so cap
        // the art smaller (26% vs 40%) to leave room for the full control stack.
        int sz = x4ColW;
        int cap = H * (smallP ? 26 : 40) / 100;
        if (sz > cap) sz = cap;
        aW = aH = sz;
        aX = x4ColX + (x4ColW - sz) / 2;
        aY = (int)(58 * s_uiscale);                  // clear the top-left back button
    } else {   // x4L
        int sz = x4ColW - (int)(56 * s_uiscale);
        if (sz > H * 25 / 100) sz = H * 25 / 100;   // larger now the card runs to the bottom
        aW = aH = sz;
        aX = x4ColX + (x4ColW - sz) / 2;             // centered in the left column
        aY = (int)(34 * s_uiscale);
    }
    art_set_bg(HOME_BG_TOP, HOME_BG_BOT, H);   // card shares the home gradient
    art_begin(scr, aX, aY, aW, aH, false);

    s_scrim = lv_obj_create(scr);
    lv_obj_remove_style_all(s_scrim);
    lv_obj_set_size(s_scrim, W, H);
    lv_obj_set_pos(s_scrim, 0, 0);
    lv_obj_set_style_bg_color(s_scrim, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_scrim, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_CLICKABLE);
    // X4 has explicit on-card controls (persistent volume, chevron, right-panel
    // toggles), so it needs no swipe gestures.
    //
    // Dark gradient scrims (fade to transparent) behind the top metadata and the
    // bottom transport/progress — guarantees white text/controls stay legible over
    // the art. Non-clickable so taps still reach s_scrim beneath.
    {
        static lv_grad_dsc_t gTop, gBot;
        gTop.dir = LV_GRAD_DIR_VER; gTop.stops_count = 2;
        gTop.stops[0].color = lv_color_black(); gTop.stops[0].opa = LV_OPA_80; gTop.stops[0].frac = 0;
        gTop.stops[1].color = lv_color_black(); gTop.stops[1].opa = LV_OPA_TRANSP; gTop.stops[1].frac = 255;
        gBot.dir = LV_GRAD_DIR_VER; gBot.stops_count = 2;
        gBot.stops[0].color = lv_color_black(); gBot.stops[0].opa = LV_OPA_TRANSP; gBot.stops[0].frac = 0;
        gBot.stops[1].color = lv_color_black(); gBot.stops[1].opa = LV_OPA_80; gBot.stops[1].frac = 255;
        lv_obj_t *tb = lv_obj_create(scr); lv_obj_remove_style_all(tb);
        lv_obj_set_size(tb, W, 96); lv_obj_set_pos(tb, 0, 0);
        lv_obj_set_style_bg_grad(tb, &gTop, 0); lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
        lv_obj_clear_flag(tb, LV_OBJ_FLAG_CLICKABLE);
        int bh = landscape ? 96 : 150;
        lv_obj_t *bb = lv_obj_create(scr); lv_obj_remove_style_all(bb);
        lv_obj_set_size(bb, W, bh); lv_obj_set_pos(bb, 0, H - bh);
        lv_obj_set_style_bg_grad(bb, &gBot, 0); lv_obj_set_style_bg_opa(bb, LV_OPA_COVER, 0);
        lv_obj_clear_flag(bb, LV_OBJ_FLAG_CLICKABLE);
    }

    // Positions scale with the font scale (s_uiscale) so the taller type on a
    // big panel doesn't overlap. X4 stacks the metadata below the art card in
    // the left column.
    const int mTitleW = x4ColW - (int)(48 * s_uiscale);
    const int mX      = x4ColX;
    const int mAX     = x4ColX;
    const int mTitleY = aY + aH + (int)(20 * s_uiscale);
    s_title  = mkLabel(scr, F24, C_TEXT, mTitleW, false);   // X4 ellipsizes (no marquee scroll)
    lv_obj_set_pos(s_title, mX, mTitleY);
    const int mTitleLH = lv_font_get_line_height(F24);
    lv_obj_set_height(s_title, mTitleLH);   // pin to ONE line so a long title doesn't wrap/overlap
    s_artist = mkLabel(scr, F16, C_SUBTLE, mTitleW, false);
    lv_obj_set_pos(s_artist, mAX, mTitleY + mTitleLH + (int)(6 * s_uiscale));
    lv_obj_set_height(s_artist, lv_font_get_line_height(F16));
    {   // top-left back "‹" on the now-playing card (in addition to the bottom chevron)
        // Chrome glyphs (back/info/volume) must not shrink with s_uiscale below a
        // readable floor — on the 240x320 a 38*0.5=19px glyph is illegible. smallP
        // pins them to fixed, finger-sized values instead.
        lv_obj_t *npback = iconBtnImg(scr, ICON_BACK, smallP ? 30 : (int)(38 * s_uiscale), 0, LV_OPA_TRANSP, 0xFFFFFF, onHomeChevron, NULL);
        lv_obj_set_pos(npback, x4M + (int)(6 * s_uiscale) + (smallP ? 4 : 0), x4M + (int)(6 * s_uiscale) + (smallP ? 4 : 0));
    }
    // x4V vertical-flow anchors (progress bar / time / transport center / volume):
    // each is the LARGER of the classic scaled offset and a font-aware minimum, so
    // the ws43 (s=1) keeps its exact spacing while the 240x320 (s<1) never overlaps
    // its fixed-height bitmap fonts. pTrPlay is the x4V transport play-button size.
    const int _lhA = lv_font_get_line_height(F16);
    const int _lhS = lv_font_get_line_height(F14);
    const int pTrPlay = (int)(74 * s_uiscale);
    int pBarY  = mTitleY + (int)(74 * s_uiscale);
    { int fa = mTitleY + mTitleLH + (int)(6 * s_uiscale) + _lhA + (int)(8 * s_uiscale); if (fa > pBarY) pBarY = fa; }
    int pTimeY = mTitleY + (int)(90 * s_uiscale);
    { int fa = pBarY + (int)(12 * s_uiscale); if (fa > pTimeY) pTimeY = fa; }
    int pTrCy  = mTitleY + (int)(160 * s_uiscale);
    { int fa = pTimeY + _lhS + (int)(12 * s_uiscale) + pTrPlay / 2; if (fa > pTrCy) pTrCy = fa; }
    int pVolY  = pTrCy + (int)(62 * s_uiscale);
    { int fa = pTrCy + pTrPlay / 2 + (int)(14 * s_uiscale); if (fa > pVolY) pVolY = fa; }
    s_time   = mkLabel(scr, F14, C_GREEN, 0, false);
    lv_obj_set_pos(s_time, mAX, x4V ? pTimeY : (mTitleY + (int)(90 * s_uiscale)));
    // total time at the progress bar's right end (elapsed is s_time, left)
    s_timeTot = mkLabel(scr, F14, C_MUTED, (int)(70 * s_uiscale), false);
    lv_obj_set_style_text_align(s_timeTot, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_timeTot, x4ColX + x4ColW - (int)(70 * s_uiscale), x4V ? pTimeY : (mTitleY + (int)(90 * s_uiscale)));
    lv_label_set_text(s_timeTot, "");
    // Until the driver sends the first state, show why the screen is blank
    // instead of a bare "-" (the driver dials in on its own schedule at boot).
    lv_label_set_text(s_title, s_haveState ? "-"
                               : (s_connected ? "Connecting\xE2\x80\xA6"
                                              : "Waiting for controller\xE2\x80\xA6"));
    // While waiting, show the SDDP name (<MODEL>-<MAC>) so the panel is
    // identifiable in Composer's discovery / when binding. Same derivation as the
    // SDDP Host (device_variant_tag), so what is on screen matches what Composer
    // lists in its Address column.
    if (!s_haveState) {
        char ident[40]; uint8_t mac[6] = {0};
        mmk_read_mac(mac);
        char tag[24];
        device_variant_tag(tag, sizeof(tag));
        snprintf(ident, sizeof(ident), "%s-%02X%02X%02X%02X%02X%02X", tag,
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        lv_label_set_text(s_artist, ident);
    } else lv_label_set_text(s_artist, "");
    lv_label_set_text(s_time, "");
    s_lastHead[0] = 0; s_lastSub[0] = 0;   // labels recreated -> invalidate the diff cache

    s_bar = lv_bar_create(scr);
    lv_bar_set_range(s_bar, 0, 1000);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(C_GREEN), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, (int)(3 * s_uiscale), 0);
    lv_obj_set_size(s_bar, x4ColW, (int)(6 * s_uiscale));
    lv_obj_set_pos(s_bar, x4ColX, x4V ? pBarY : (mTitleY + (int)(74 * s_uiscale)));

    // Transport: create all five controls; layoutTransport() then shows + centers the
    // subset the source supports (set from caps in setState). X4 transport is plain
    // glyphs (no button circles/pills) on the card/gradient background.
    {
        uint32_t sideBg  = 0x000000;
        lv_opa_t sideOpa = LV_OPA_TRANSP;
        uint32_t playBg  = 0x000000;
        lv_opa_t playOpa = LV_OPA_TRANSP;
        uint32_t sideFg = 0xFFFFFF, playFg = 0xFFFFFF;   // white glyphs
        if (x4V) {   // portrait/small-landscape: big touch targets, centred full-width below the meta
            s_trPlay = pTrPlay;
            s_trSide = (int)(58 * s_uiscale);
            s_trCx   = x4ColX + x4ColW / 2;
            s_trMaxW = x4ColW;
            s_trCy   = pTrCy;   // font-aware: clears the time row on the small panel
        } else {   // x4L: plain glyphs on the navy bg, centred in the left column
            s_trPlay = (int)(44 * s_uiscale);
            s_trSide = (int)(38 * s_uiscale);
            s_trCx   = x4ColX + x4ColW / 2;
            s_trMaxW = x4ColW;
            s_trCy   = mTitleY + (int)(128 * s_uiscale);
        }
        s_shufBtn   = iconBtnImg(scr, ICON_SHUFFLE,    s_trSide, sideBg, sideOpa, sideFg, onShuffle,   &s_shufIcon);
        s_thumbDown = iconBtnImg(scr, ICON_THUMB_DOWN, s_trSide, sideBg, sideOpa, sideFg, onThumbDown, NULL);
        s_prev      = iconBtnImg(scr, ICON_SKIP_PREV,  s_trSide, sideBg, sideOpa, sideFg, onPrev,      NULL);
        s_play      = iconBtnImg(scr, ICON_PAUSE,      s_trPlay, playBg, playOpa, playFg, onPlayPause, &s_ppIcon);
        s_next      = iconBtnImg(scr, ICON_SKIP_NEXT,  s_trSide, sideBg, sideOpa, sideFg, onNext,      NULL);
        s_thumbUp   = iconBtnImg(scr, ICON_THUMB_UP,   s_trSide, sideBg, sideOpa, sideFg, onThumbUp,   NULL);
        s_repBtn    = iconBtnImg(scr, ICON_REPEAT,     s_trSide, sideBg, sideOpa, sideFg, onRepeat,    &s_repIcon);
        layoutTransport();
    }

    // Overflow menu: a plain vertical ellipsis (⋮) at the title's right, like
    // Navigator — opens the (i) info panel. (No pill background.)
    { int isz = smallP ? 30 : (int)(34 * s_uiscale);
    s_infoToggle = iconBtnImg(scr, ICON_DOTS, isz, 0x000000, LV_OPA_TRANSP, 0xFFFFFF, onInfo, NULL);
    lv_obj_set_pos(s_infoToggle, x4ColX + x4ColW - isz, mTitleY + (int)(2 * s_uiscale)); }

    // No top-left intercom icon on X4: the home screen has its own Intercom card,
    // so a stray now-playing shortcut would be redundant. s_icToggle stays NULL —
    // the guards on it elsewhere are harmless no-ops.

    // X4 card bottom stack: persistent volume, then the house+/"Playing in <room>"
    // row (also the add-rooms trigger) + power, then the collapse chevron pinned to
    // the card bottom. Anchored so nothing overlaps.
    {
        const float s = s_uiscale;
        // Landscape card runs to the bottom edge; portrait keeps its bottom margin.
        const int cardBot = x4V ? (H - x4M - (int)(16 * s)) : (H - (int)(18 * s));

        // Persistent volume row (speaker + slider), below the transport (portrait's
        // transport is taller, so it needs a larger gap to clear it).
        const int volY = x4V ? pVolY : (s_trCy + (int)(44 * s));
        const int viSz  = smallP ? 26 : (int)(24 * s);           // readable speaker glyph
        const int viGap = smallP ? 36 : (int)(44 * s);           // slider offset past the icon
        lv_obj_t *nvi = lv_image_create(scr);
        lv_image_set_src(nvi, ICON_VOL_UP);
        lv_obj_set_size(nvi, viSz, viSz);
        lv_image_set_inner_align(nvi, LV_IMAGE_ALIGN_CONTAIN);
        lv_obj_set_style_image_recolor(nvi, lv_color_hex(C_TEXT), 0);
        lv_obj_set_style_image_recolor_opa(nvi, LV_OPA_COVER, 0);
        lv_obj_set_pos(nvi, x4ColX, volY);
        s_npVolIcon = nvi;
        s_npVol = lv_slider_create(scr);
        lv_slider_set_range(s_npVol, 0, 100);
        lv_obj_set_size(s_npVol, x4ColW - viGap, smallP ? 8 : (int)(10 * s));
        lv_obj_set_pos(s_npVol, x4ColX + viGap, volY + (smallP ? 9 : (int)(7 * s)));
        lv_obj_set_ext_click_area(s_npVol, (int)(18 * s));
        lv_obj_set_style_bg_color(s_npVol, lv_color_hex(0x404040), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_npVol, lv_color_hex(C_TEXT), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_npVol, lv_color_hex(C_TEXT), LV_PART_KNOB);
        lv_obj_add_event_cb(s_npVol, onNpVol, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(s_npVol, onNpVol, LV_EVENT_VALUE_CHANGED, NULL);

        // house+ / "Playing in <room>" (opens the room list) + power + collapse
        // chevron. Skipped on the tiny 240x320 (smallP): the room is on Home and the
        // top-left back button already collapses — no vertical room for this stack.
        if (!smallP) {
        const int botY = volY + (int)(42 * s);
        lv_obj_t *hr = iconBtnImg(scr, ICON_ROOM_ADD, (int)(34 * s), 0, LV_OPA_TRANSP, 0xFFFFFF, onHomeAddRooms, NULL);
        lv_obj_set_pos(hr, x4ColX, botY);
        lv_obj_t *pin = lv_label_create(scr);
        lv_obj_set_style_text_font(pin, F14, 0);
        lv_obj_set_style_text_color(pin, lv_color_hex(C_MUTED), 0);
        lv_label_set_text(pin, "Playing in");
        lv_obj_set_pos(pin, x4ColX + (int)(46 * s), botY);
        s_npRoomLbl = lv_label_create(scr);
        lv_obj_set_style_text_font(s_npRoomLbl, F16, 0);
        lv_obj_set_style_text_color(s_npRoomLbl, lv_color_hex(C_TEXT), 0);
        lv_label_set_text(s_npRoomLbl, s_lastState.room[0] ? s_lastState.room : "");
        lv_obj_set_pos(s_npRoomLbl, x4ColX + (int)(46 * s), botY + (int)(18 * s));
        lv_obj_t *pwr = iconBtnImg(scr, ICON_POWER, (int)(34 * s), 0, LV_OPA_TRANSP, 0xFFFFFF, onHomePower, NULL);
        lv_obj_set_pos(pwr, x4ColX + x4ColW - (int)(38 * s), botY);

        // Collapse chevron, pinned to the card bottom (below the bottom row).
        lv_obj_t *chev = iconBtnImg(scr, ICON_CHEVRON_DOWN, (int)(32 * s),
                                    0x000000, LV_OPA_TRANSP, 0xFFFFFF, onHomeChevron, NULL);
        lv_obj_set_pos(chev, x4ColX + x4ColW / 2 - (int)(16 * s), cardBot - (int)(34 * s));
        } else s_npRoomLbl = NULL;   // smallP: no room row
    }


    // Track-info panel: the card's RIGHT half on x4L (transparent, an (i) header,
    // two-line gray-name/white-value rows with dividers), or a full-card dark
    // overlay on x4V (portrait/small-landscape — tap anywhere to close).
    {
        const float s = s_uiscale;
        s_infoPanel = lv_obj_create(scr);
        lv_obj_remove_style_all(s_infoPanel);
        lv_obj_add_flag(s_infoPanel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_infoPanel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *rowsWrap = lv_obj_create(s_infoPanel);
        lv_obj_remove_style_all(rowsWrap);
        lv_obj_set_flex_flow(rowsWrap, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(rowsWrap, (int)(12 * s), 0);
        lv_obj_set_scroll_dir(rowsWrap, LV_DIR_VER);
        int ix, iy, iw, ipH;
        if (x4V) {   // portrait/small-landscape: full-card dark overlay (tap anywhere to close)
            ix = x4M; iy = x4M; iw = W - 2 * x4M; ipH = H - 2 * x4M;
            lv_obj_set_style_bg_color(s_infoPanel, lv_color_hex(0x0E1120), 0);
            lv_obj_set_style_bg_opa(s_infoPanel, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(s_infoPanel, (int)(26 * s), 0);
            lv_obj_add_flag(s_infoPanel, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(s_infoPanel, onInfo, LV_EVENT_CLICKED, NULL);
        } else {   // x4L: card's right half (transparent)
            ix = W / 2; iy = x4M + (int)(28 * s);
            iw = W - x4M - (int)(24 * s) - ix; ipH = H - x4M - iy - (int)(16 * s);
        }
        lv_obj_set_size(s_infoPanel, iw, ipH);
        lv_obj_set_pos(s_infoPanel, ix, iy);
        lv_obj_set_style_pad_all(s_infoPanel, x4V ? (int)(18 * s) : 0, 0);
        lv_obj_t *ici = lv_image_create(s_infoPanel);   // (i) header icon, centered
        lv_image_set_src(ici, ICON_INFO);
        lv_obj_set_size(ici, (int)(46 * s), (int)(46 * s));
        lv_image_set_inner_align(ici, LV_IMAGE_ALIGN_CONTAIN);
        lv_obj_set_style_image_recolor(ici, lv_color_hex(C_TEXT), 0);
        lv_obj_set_style_image_recolor_opa(ici, LV_OPA_COVER, 0);
        lv_obj_align(ici, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_size(rowsWrap, LV_PCT(100), ipH - (int)(72 * s));
        lv_obj_align(rowsWrap, LV_ALIGN_TOP_LEFT, 0, (int)(72 * s));
        for (int i = 0; i < 8; i++) {
            lv_obj_t *row = lv_obj_create(rowsWrap);
            lv_obj_remove_style_all(row);
            lv_obj_set_width(row, LV_PCT(100));
            lv_obj_set_height(row, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(row, (int)(2 * s), 0);
            lv_obj_set_style_pad_bottom(row, (int)(7 * s), 0);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_color(row, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_border_opa(row, LV_OPA_20, 0);
            lv_obj_set_style_border_width(row, 1, 0);
            s_infoName[i] = lv_label_create(row);
            lv_obj_set_style_text_font(s_infoName[i], F14, 0);
            lv_obj_set_style_text_color(s_infoName[i], lv_color_hex(C_MUTED), 0);
            s_infoVal[i] = lv_label_create(row);
            lv_obj_set_style_text_font(s_infoVal[i], F16, 0);
            lv_obj_set_style_text_color(s_infoVal[i], lv_color_hex(C_TEXT), 0);
            lv_obj_set_width(s_infoVal[i], LV_PCT(100));
            lv_label_set_long_mode(s_infoVal[i], LV_LABEL_LONG_DOT);
            s_infoRows[i] = row;
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // X4 add-rooms right panel (shown by the house+ control). Master volume + a room
    // list; the list is stubbed to just this room until the driver enumerates rooms.
    if (x4) {
        const float s = s_uiscale;
        int ix, iy, iw, ipH;
        if (x4V) { ix = x4M; iy = x4M; iw = W - 2 * x4M; ipH = H - 2 * x4M; }
        else     { ix = W / 2; iy = x4M + (int)(28 * s);
                   iw = W - x4M - (int)(24 * s) - ix; ipH = H - x4M - iy - (int)(16 * s); }
        s_roomsPanel = lv_obj_create(scr);
        lv_obj_remove_style_all(s_roomsPanel);
        lv_obj_set_size(s_roomsPanel, iw, ipH);
        lv_obj_set_pos(s_roomsPanel, ix, iy);
        // Padding + a column layout. This panel used to place every child with
        // absolute lv_obj_align offsets against a style-less (zero-padding) parent,
        // which put the volume slider hard against the top edge and running PAST the
        // right edge, and left the list rows and their state dots flush to the panel
        // border. Flex + real padding keeps everything inside the card at any size.
        {
            int rpad = (int)(16 * s);
            lv_obj_set_style_pad_all(s_roomsPanel, rpad, 0);
            lv_obj_set_style_pad_row(s_roomsPanel, (int)(10 * s), 0);
            lv_obj_set_flex_flow(s_roomsPanel, LV_FLEX_FLOW_COLUMN);
        }
        if (x4V) {   // portrait: full-card dark overlay
            lv_obj_set_style_bg_color(s_roomsPanel, lv_color_hex(0x0E1120), 0);
            lv_obj_set_style_bg_opa(s_roomsPanel, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(s_roomsPanel, (int)(26 * s), 0);
        }
        lv_obj_add_flag(s_roomsPanel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_roomsPanel, LV_OBJ_FLAG_SCROLLABLE);

        // Header: an explicit BACK control + title. Previously there was no way out
        // of this screen at all in landscape, and in portrait the only exit was a tap
        // on bare panel background -- which the room list covers, so a stray tap hit a
        // room row and silently joined/left that room instead of closing.
        {
            lv_obj_t *hdr = lv_obj_create(s_roomsPanel);
            lv_obj_remove_style_all(hdr);
            lv_obj_set_width(hdr, LV_PCT(100));
            lv_obj_set_height(hdr, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(hdr, (int)(10 * s), 0);
            int bsz = (s < 0.99f) ? 30 : (int)(30 * s);
            iconBtnImg(hdr, ICON_BACK, bsz, C_BTN, LV_OPA_COVER, 0xFFFFFF, onRoomsClose, NULL);
            lv_obj_t *ht = lv_label_create(hdr);
            lv_label_set_text(ht, "Rooms");
            lv_obj_set_style_text_color(ht, lv_color_hex(C_TEXT), 0);
            lv_obj_set_style_text_font(ht, F24, 0);
        }

        // Master volume row: icon + slider, the slider taking the remaining width.
        {
            lv_obj_t *vrow = lv_obj_create(s_roomsPanel);
            lv_obj_remove_style_all(vrow);
            lv_obj_set_width(vrow, LV_PCT(100));
            lv_obj_set_height(vrow, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(vrow, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(vrow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(vrow, (int)(12 * s), 0);
            lv_obj_set_style_pad_ver(vrow, (int)(6 * s), 0);

            lv_obj_t *rvi = lv_image_create(vrow);   // master volume speaker icon
            lv_image_set_src(rvi, ICON_VOL_UP);
            lv_obj_set_size(rvi, (int)(28 * s), (int)(28 * s));
            lv_image_set_inner_align(rvi, LV_IMAGE_ALIGN_CONTAIN);
            lv_obj_set_style_image_recolor(rvi, lv_color_hex(C_TEXT), 0);
            lv_obj_set_style_image_recolor_opa(rvi, LV_OPA_COVER, 0);
            s_roomsVolIcon = rvi;

            s_roomsVol = lv_slider_create(vrow);
            lv_slider_set_range(s_roomsVol, 0, 100);
            lv_obj_set_height(s_roomsVol, (int)(10 * s));
            lv_obj_set_flex_grow(s_roomsVol, 1);      // fills the row, never overflows
            lv_obj_set_ext_click_area(s_roomsVol, (int)(18 * s));
            lv_obj_set_style_bg_color(s_roomsVol, lv_color_hex(0x404040), LV_PART_MAIN);
            lv_obj_set_style_bg_color(s_roomsVol, lv_color_hex(C_TEXT), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(s_roomsVol, lv_color_hex(C_TEXT), LV_PART_KNOB);
            lv_obj_add_event_cb(s_roomsVol, onRoomsVol, LV_EVENT_RELEASED, NULL);
            lv_obj_add_event_cb(s_roomsVol, onRoomsVol, LV_EVENT_VALUE_CHANGED, NULL);
        }

        lv_obj_t *list = lv_obj_create(s_roomsPanel);
        lv_obj_remove_style_all(list);
        lv_obj_set_width(list, LV_PCT(100));
        lv_obj_set_flex_grow(list, 1);          // takes whatever the header/volume leave
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scroll_dir(list, LV_DIR_VER);
        s_roomsList = list;   // ui_set_rooms rebuilds its rows from the driver's `rooms`

        lv_obj_t *row = lv_obj_create(list);   // this-room row (active)
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, (int)(54 * s));
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_opa(row, LV_OPA_20, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        s_roomsRowLbl = lv_label_create(row);
        lv_obj_set_style_text_font(s_roomsRowLbl, F16, 0);
        lv_obj_set_style_text_color(s_roomsRowLbl, lv_color_hex(C_TEXT), 0);
        lv_label_set_text(s_roomsRowLbl, s_lastState.room[0] ? s_lastState.room : "This Room");
        lv_obj_align(s_roomsRowLbl, LV_ALIGN_LEFT_MID, 0, 0);
        // Filled selection circle -- this room is always in its own session. Must
        // match the circles ui_set_rooms draws (same size/treatment), since this
        // stub row sits at the top of that same list until the driver replies.
        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, (int)(24 * s), (int)(24 * s));
        lv_obj_align(dot, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 2, 0);
        lv_obj_set_style_border_color(dot, lv_color_hex(C_TEXT), 0);
        lv_obj_set_style_border_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(C_TEXT), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    }

    // Intercom picker overlay: scrollable list of callable targets (groups + endpoints).
    s_icPanel = lv_obj_create(scr);
    lv_obj_remove_style_all(s_icPanel);
    lv_obj_set_size(s_icPanel, W, H);   // X4: full-screen page ("‹ Intercom" + list)
    lv_obj_set_pos(s_icPanel, 0, 0);
    lv_obj_add_flag(s_icPanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(s_icPanel, lv_color_hex(HOME_BG_TOP), 0);
    lv_obj_set_style_bg_grad_color(s_icPanel, lv_color_hex(HOME_BG_BOT), 0);
    lv_obj_set_style_bg_grad_dir(s_icPanel, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_icPanel, LV_OPA_COVER, 0);
    if (smallP) {
        // Tiny 2.8": no "Intercom" heading — small back chevron only (matches
        // the keypad page); the list moves up into the reclaimed space below.
        lv_obj_t *back = iconBtnImg(s_icPanel, ICON_BACK, 30, 0, LV_OPA_TRANSP, 0xFFFFFF, onIcHome, NULL);
        lv_obj_align(back, LV_ALIGN_TOP_LEFT, 8, 8);
    } else {
        x4PageHeader(s_icPanel, "Intercom", onIcHome);
    }
    const int icPad = smallP ? 12 : (int)(28 * s_uiscale);
    const int icTop = smallP ? 44 : (int)(118 * s_uiscale);
    s_icList = lv_obj_create(s_icPanel);
    lv_obj_remove_style_all(s_icList);
    lv_obj_set_size(s_icList, W - 2 * icPad, H - icTop - (int)(24 * s_uiscale));
    lv_obj_set_pos(s_icList, icPad, icTop);
    lv_obj_set_style_pad_all(s_icList, 0, 0);
    lv_obj_set_style_pad_row(s_icList, 0, 0);   // x4 rows use dividers, no gap
    lv_obj_set_flex_flow(s_icList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_icList, LV_DIR_VER);
    rebuildIcList();
    if (s_nEps > 0 && s_icToggle) lv_obj_clear_flag(s_icToggle, LV_OBJ_FLAG_HIDDEN);   // survive a layout rebuild

    // Favorites grid overlay: full-screen page ("‹ Favorites" + a wrapping tile grid),
    // populated on demand from the driver's `favorites` reply (ui_set_favorites). Same
    // full-screen page pattern as the intercom picker.
    s_favPanel = lv_obj_create(scr);
    lv_obj_remove_style_all(s_favPanel);
    lv_obj_set_size(s_favPanel, W, H);
    lv_obj_set_pos(s_favPanel, 0, 0);
    lv_obj_add_flag(s_favPanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(s_favPanel, lv_color_hex(HOME_BG_TOP), 0);
    lv_obj_set_style_bg_grad_color(s_favPanel, lv_color_hex(HOME_BG_BOT), 0);
    lv_obj_set_style_bg_grad_dir(s_favPanel, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_favPanel, LV_OPA_COVER, 0);
    if (smallP) {
        lv_obj_t *back = iconBtnImg(s_favPanel, ICON_BACK, 30, 0, LV_OPA_TRANSP, 0xFFFFFF, onFavHome, NULL);
        lv_obj_align(back, LV_ALIGN_TOP_LEFT, 8, 8);
    } else {
        x4PageHeader(s_favPanel, "Favorites", onFavHome);
    }
    const int fvPad = smallP ? 12 : (int)(28 * s_uiscale);
    const int fvTop = smallP ? 44 : (int)(118 * s_uiscale);
    s_favGrid = lv_obj_create(s_favPanel);
    lv_obj_remove_style_all(s_favGrid);
    lv_obj_set_size(s_favGrid, W - 2 * fvPad, H - fvTop - (int)(24 * s_uiscale));
    lv_obj_set_pos(s_favGrid, fvPad, fvTop);
    lv_obj_set_style_pad_all(s_favGrid, 0, 0);
    lv_obj_set_style_pad_row(s_favGrid, (int)(14 * s_uiscale), 0);
    lv_obj_set_style_pad_column(s_favGrid, (int)(14 * s_uiscale), 0);
    lv_obj_set_flex_flow(s_favGrid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_scroll_dir(s_favGrid, LV_DIR_VER);
    rebuildFavGrid();

    // X4-inspired home screen on top of the now-playing (shown by default). Its
    // own rich indigo->blue gradient (not the charcoal now-playing bg) so the
    // dark glass cards stand out.
    if (x4) {
        build_home(scr, W, H, HOME_BG_TOP, HOME_BG_BOT);
        if (!s_homeAtHome) lv_obj_add_flag(s_home, LV_OBJ_FLAG_HIDDEN);
    }

    // The settings menu lives on the Home panel (build_home) so it only shows on
    // the main room page — not on now-playing or the keypad sub-page.
}

// Progress is shown by the bar alone (no elapsed/remaining text). The bar fills from a
// local clock (the driver doesn't report track position), gated on a known duration.
static void updateTime(void)
{
    bool have = (s_duration > 0 && s_showProgress);
    if (s_bar) setVis(s_bar, have);
    // X4 shows elapsed (green, left) + total (gray, right) at the bar ends.
    setVis(s_time, have);
    setVis(s_timeTot, have);
    if (have) {
        lv_label_set_text_fmt(s_time, "%d:%02d", s_elapsed / 60, s_elapsed % 60);
        if (s_timeTot) lv_label_set_text_fmt(s_timeTot, "%d:%02d", s_duration / 60, s_duration % 60);
    }
    if (!have) return;
    lv_bar_set_value(s_bar, (int)((long)s_elapsed * 1000 / s_duration), LV_ANIM_OFF);
}


// Driver TCP link up/down. Until the first state arrives, reflect it in the
// title so the boot gap (waiting for the driver to dial in) is explained.
void ui_set_connected(bool connected)
{
    bool was = s_connected;
    s_connected = connected;
    // The landing page renders a different body depending on the link (action cards vs
    // the not-connected notice), so a change has to re-render it. Only once the UI
    // actually exists: a rebuild during boot/splash is both pointless (ui_begin has not
    // run, or is about to) and was the trigger that exposed the splash use-after-free.
    if (was != connected && s_home) ui_request_rebuild();
    // Ask for this room's favourites once per connect. Deliberately NOT from
    // build_home(): the reply rebuilds the page, and a rebuild re-runs build_home, so
    // requesting there is a loop -- it re-rendered the UI about once a second and
    // looked like the panel was crashing.
    if (connected && !was) net_request_favorites();
    if (!s_haveState && s_title)
        lv_label_set_text(s_title, connected ? "Connecting\xE2\x80\xA6"
                                             : "Waiting for controller\xE2\x80\xA6");
}

void ui_set_state(const media_state_t *st)
{
    s_lastState = *st; s_haveState = true;
    bool hasSource = st->source.id[0] && strcmp(st->source.id, "0") != 0;
    if (s_ppHoldUntil && (int32_t)(millis() - s_ppHoldUntil) < 0) {
        if (st->playing == s_ppExpect) s_ppHoldUntil = 0;
    } else {
        s_ppHoldUntil = 0;
        s_lastPlaying = st->playing;
    }
    // Only a non-empty, different title counts as a new track. Some sources blank the
    // title while paused; without this guard that reset the progress bar on resume.
    bool songChanged = (st->title[0] && strcmp(st->title, s_progTitle) != 0);
    if (songChanged) { s_loadUntil = 0; setDim(false); }
    s_keepAwake = s_lastPlaying || (st->power && hasSource);

    s_showTitle = st->show_title;     s_showArtist   = st->show_artist;
    s_showInfo  = st->show_info;      s_showProgress = st->show_progress;
    s_stopMode  = (st->can_stop && !st->can_pause);
    s_cPrimary  = (st->can_pause || st->can_stop);   // no play/pause/stop -> hide center (Mac etc.)
    s_cPrev = st->can_prev; s_cNext = st->can_next;
    s_cThUp = st->can_thumbs_up; s_cThDn = st->can_thumbs_down;
    s_cShuffle = st->can_shuffle; s_cRepeat = st->can_repeat;
    // Authoritative state from the driver, unless we tapped within the last ~2s (let the
    // optimistic flip stand until the source's state catches up, avoiding a flicker).
    if (!s_cShuffle) s_shufOn = false; else if (millis() - s_shufTapMs > 2000) s_shufOn = st->shuffle_on;
    if (!s_cRepeat)  s_repOn  = false; else if (millis() - s_repTapMs  > 2000) s_repOn  = st->repeat_on;
    styleToggle(s_shufIcon, s_shufOn);
    styleToggle(s_repIcon,  s_repOn);
    layoutTransport();   // caps may have changed -> re-derive the visible row + positions

    char head[96], sub[192];
    const char *src = st->title[0] ? st->title : (st->source.name[0] ? st->source.name : (st->power ? "On" : "Off"));
    sanitize(src, head, sizeof(head));
    // #15: only re-set the label when the text actually changed — avoids restarting the
    // circular scroll animation on every 1 Hz state push.
    if (strcmp(head, s_lastHead) != 0) {
        lv_label_set_text(s_title, head); strcpy(s_lastHead, head);
        // The compact bar owns this label via the rotation -- setting it here too
        // would snap it back to the title mid-cycle on every 1 Hz state push.
        // A new track restarts the cycle from the title.
        if (s_miniRotTimer) { s_miniRot = 0; miniRotApply(false); }
        else if (s_homeMiniTitle) lv_label_set_text(s_homeMiniTitle, head);
    }
    setVis(s_title, s_showTitle);
    sanitize(st->artist, sub, sizeof(sub));
    if (st->album[0]) {
        char al[96]; sanitize(st->album, al, sizeof(al));
        if (al[0]) {
            if (sub[0]) strncat(sub, "  -  ", sizeof(sub) - strlen(sub) - 1);
            strncat(sub, al, sizeof(sub) - strlen(sub) - 1);
        }
    }
    if (strcmp(sub, s_lastSub) != 0) {
        lv_label_set_text(s_artist, sub); strcpy(s_lastSub, sub);
        if (s_homeMiniArtist) lv_label_set_text(s_homeMiniArtist, sub);   // mini-player artist
        if (s_miniRotTimer) miniRotApply(false);   // artist/album may have changed
    }
    setVis(s_artist, s_showArtist);
    if (s_ppIcon) lv_image_set_src(s_ppIcon, s_lastPlaying ? ICON_PAUSE : ICON_PLAY);
    if (s_homeMiniPP) lv_image_set_src(s_homeMiniPP, s_lastPlaying ? ICON_PAUSE : ICON_PLAY); // stopMode -> play glyph for now

    s_duration = st->duration;
    // Progress position: the bar runs on a local 1 Hz clock (ui_tick_progress), but
    // SEED/RESYNC it from the driver when it reports a real position (>0) — so a fresh
    // track, a seek, or joining mid-playback shows the right spot instead of starting
    // from 0. A reported 0 means "unknown" → keep the local clock (degrades to before).
    int seed = st->position;
    if (seed > 0 && s_duration > 0 && seed > s_duration) seed = s_duration;  // clamp
    if (songChanged) {
        strncpy(s_progTitle, st->title, sizeof(s_progTitle) - 1); s_progTitle[sizeof(s_progTitle)-1]=0;
        s_elapsed = (seed > 0) ? seed : 0;
        s_progMs = millis();
    } else if (seed > 0) {
        int drift = seed - s_elapsed; if (drift < 0) drift = -drift;
        if (drift > 3) { s_elapsed = seed; s_progMs = millis(); }   // resync on seek/join
    }
    updateTime();

    if (s_homeVol && !lv_obj_has_state(s_homeVol, LV_STATE_PRESSED)) {   // home mini-player volume
        lv_slider_set_value(s_homeVol, st->volume, LV_ANIM_OFF);
        if (s_homeVolIcon)
            lv_image_set_src(s_homeVolIcon, (st->muted || st->volume == 0) ? ICON_VOL_MUTE : ICON_VOL_UP);
    }
    if (s_npVol && !lv_obj_has_state(s_npVol, LV_STATE_PRESSED)) {   // X4 card volume
        lv_slider_set_value(s_npVol, st->volume, LV_ANIM_OFF);
        if (s_npVolIcon)
            lv_image_set_src(s_npVolIcon, (st->muted || st->volume == 0) ? ICON_VOL_MUTE : ICON_VOL_UP);
    }
    if (s_roomsVol && !lv_obj_has_state(s_roomsVol, LV_STATE_PRESSED)) {   // X4 add-rooms master volume
        lv_slider_set_value(s_roomsVol, st->volume, LV_ANIM_OFF);
        if (s_roomsVolIcon)
            lv_image_set_src(s_roomsVolIcon, (st->muted || st->volume == 0) ? ICON_VOL_MUTE : ICON_VOL_UP);
    }
    if (st->room[0]) {   // "Playing in <room>" + the room-list row name + landing title
        if (s_npRoomLbl)   lv_label_set_text(s_npRoomLbl, st->room);
        if (s_roomsRowLbl) lv_label_set_text(s_roomsRowLbl, st->room);
        if (s_homeTitle)   lv_label_set_text(s_homeTitle, st->room);
    }

    // When the media session ENDS, return to the panel's RESTING view. Without this
    // the panel was left on whatever sub-page the session had taken it to, showing
    // controls for something that is no longer playing.
    //
    //
    // Only on the transition, so it never pulls the user out of a page they opened
    // themselves while nothing is playing.
    {
        bool session = st->power && (st->title[0] || !strcmp(st->media_type, "media"));
        if (s_hadSession && !session) {
            if (s_icPanel)    lv_obj_add_flag(s_icPanel,    LV_OBJ_FLAG_HIDDEN);
            if (s_favPanel)   lv_obj_add_flag(s_favPanel,   LV_OBJ_FLAG_HIDDEN);
            if (s_infoPanel)  lv_obj_add_flag(s_infoPanel,  LV_OBJ_FLAG_HIDDEN);
            if (s_roomsPanel) lv_obj_add_flag(s_roomsPanel, LV_OBJ_FLAG_HIDDEN);
            showHome();
        }
        s_hadSession = session;
    }
    art_load(st->art_url);

    for (int i = 0; i < 8; i++) {
        if (!s_infoRows[i]) continue;
        if (i < st->n_meta) {
            if (s_infoName[i]) lv_label_set_text(s_infoName[i], st->meta[i].id);
            if (s_infoVal[i])  lv_label_set_text(s_infoVal[i], st->meta[i].name);
            lv_obj_clear_flag(s_infoRows[i], LV_OBJ_FLAG_HIDDEN);
        } else lv_obj_add_flag(s_infoRows[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Whether the driver has any buttons configured; they render as tiles on the
    // room page (build_home_tiles).
    bool haveButtons = (st->n_buttons > 0);
    s_haveButtons = haveButtons;
    if (s_homeBar) setVis(s_homeBar, st->power);   // hide the mini-player when the room is off
    if ((int)st->power != (int)s_lastPower) s_lastPower = st->power;
}

// Zero every widget pointer (review CRITICAL fix): lv_obj_clean frees them all, so
// after a rebuild nothing must hold a dangling pointer before begin() recreates it.
static void clearWidgets(void)
{
    s_scrim = s_title = s_artist = s_time = s_bar = s_ppIcon = s_timeTot = NULL;
    s_npCard = s_npVol = s_npVolIcon = NULL;
    s_roomsPanel = s_roomsVol = s_roomsVolIcon = s_npRoomLbl = s_roomsRowLbl = NULL;
    s_nRooms = 0;   // the room rows are destroyed with the panel; drop stale indices
    s_play = s_prev = s_next = s_thumbUp = s_thumbDown = s_shufBtn = s_repBtn = NULL;
    s_shufIcon = s_repIcon = NULL;
    s_infoPanel = s_infoToggle = NULL;
    s_icPanel = s_icList = s_icToggle = NULL;   // endpoints (s_eps/s_nEps) persist across rebuild
    s_favPanel = s_favGrid = NULL;
    // NB: s_favs / s_nFavs deliberately SURVIVE a rebuild. They used to be cleared here
    // because favourites lived in a sub-page that re-requested them every time it was
    // opened. They are now rows on the landing page and the list is requested once per
    // connect, so clearing it here meant every rebuild silently dropped the favourites
    // and build_home had nothing to draw.
    if (s_miniRotTimer) { lv_timer_delete(s_miniRotTimer); s_miniRotTimer = NULL; }
    s_home = s_homeMiniTitle = s_homeMiniArtist = s_homeMiniArt = s_homeMiniPP = NULL;
    s_homeVol = s_homeVolIcon = s_homeMiniNote = NULL;
    s_homeBar = NULL;
    for (int i = 0; i < 8; i++) s_infoRows[i] = s_infoName[i] = s_infoVal[i] = NULL;
    s_homeTitle = NULL;   // rebuilt by build_home; must not dangle across a rebuild
}

static void doRebuild(void)
{
    art_detach();
    art_thumb_clear();   // the grid's canvases die with the UI; free their buffers
    // The boot splash is a child of the active screen, so lv_obj_clean() below frees
    // it -- leaving s_splashCv dangling for splash_done()'s timer to delete a second
    // time. That is a use-after-free and it panics the board (Load access fault in
    // lv_obj_delete). Latent for as long as the splash has existed: it only needed a
    // rebuild inside the ~1.8s splash window, which nothing used to do. Tear it down
    // here so the timer finds NULL and no-ops.
    if (s_splashCv)  { lv_obj_delete(s_splashCv); s_splashCv = NULL; }
    if (s_splashBuf) { heap_caps_free(s_splashBuf); s_splashBuf = NULL; }
    lv_obj_clean(lv_screen_active());
    clearWidgets();
    s_lastPower = true; s_progTitle[0] = 0;
    ui_begin();
    if (s_haveState) ui_set_state(&s_lastState);
}
void ui_request_rebuild(void) { s_rebuildReq = true; }

void ui_tick_progress(void)
{
    if (s_rebuildReq) { s_rebuildReq = false; doRebuild(); return; }
    art_tick();
    art_thumb_tick();   // publish any favourite-tile artwork that finished decoding
    // Mini-player art tile: show real cover art only for an actual track (room on +
    // a title). art.c also decodes bare source-tile logos (e.g. the Apple Music icon),
    // which are NOT album art — keep the music-note placeholder for those instead.
    bool realArt = art_has() && s_haveState && s_lastState.power && s_lastState.title[0];
    if (s_homeMiniNote) setVis(s_homeMiniNote, !realArt);
    if (s_loadUntil && millis() > s_loadUntil) { s_loadUntil = 0; setDim(false); }
    // Reconcile the optimistic play/pause icon once the hold window lapses, so it
    // can't get stuck if the driver stops pushing (review fix).
    if (!s_ppHoldUntil && s_haveState && s_ppIcon && !s_stopMode) {
        bool real = s_lastState.playing;
        if (real != s_lastPlaying) {
            s_lastPlaying = real;
            lv_image_set_src(s_ppIcon, real ? ICON_PAUSE : ICON_PLAY);
            if (s_homeMiniPP) lv_image_set_src(s_homeMiniPP, real ? ICON_PAUSE : ICON_PLAY);
        }
    }
    if (s_lastPlaying && s_duration > 0 && (millis() - s_progMs) >= 1000) {
        s_progMs += 1000;
        if (s_elapsed < s_duration) s_elapsed++;
        updateTime();
    }
}

void ui_tick_screensaver(void)
{
    // Periodic favourites refresh. The list is otherwise only fetched on connect, so a
    // favourite added or renamed in Composer would not appear until the link bounced.
    // ui_set_favorites() only rebuilds the page when the CONTENT actually changed, so a
    // no-op refresh costs one small round trip and nothing else.
    static uint32_t s_favTick;
    if (s_connected && ++s_favTick >= FAV_REFRESH_SEC) { s_favTick = 0; net_request_favorites(); }

    // The intercom card is gated on ic_available(); rebuild when that flips vs
    // what home currently shows.
    if (s_home && ic_available() != s_homeIcShown) { ui_request_rebuild(); return; }

    uint16_t ss = g_settings.screensaver_sec;
    static int applied = -1;   // last backlight % pushed; -1 = none yet
    uint32_t idle = lv_display_get_inactive_time(NULL);
    bool shouldDim = (ss > 0) && !s_keepAwake && !s_call && (idle > (uint32_t)ss * 1000);
    // Desired level: idle dim level (may be 0 = screen off) when idle, else active
    // brightness. Tracking the applied value means live driver/web changes to either
    // level — and waking on touch (idle resets) — apply on the next tick.
    int want = shouldDim ? g_settings.dim_brightness : g_settings.brightness;
    if (want != applied) { bsp_set_backlight((uint8_t)want); applied = want; }
    // Arm the wake shield exactly while dimmed. onWakeTap drops it on the press,
    // so it never eats a second tap.
    if (s_wakeShield) {
        if (shouldDim) lv_obj_remove_flag(s_wakeShield, LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag(s_wakeShield, LV_OBJ_FLAG_HIDDEN);
    }
}

static void identify_done(lv_timer_t *tm)
{
    lv_obj_delete((lv_obj_t *)lv_timer_get_user_data(tm));
    lv_timer_delete(tm);
}
void ui_identify(void)
{
    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ov, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_t *l = lv_label_create(ov);
    lv_label_set_text(l, "M Keypad");
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(l, F32, 0);
    lv_obj_center(l);
    lv_timer_t *t = lv_timer_create(identify_done, 3000, ov);
    lv_timer_set_repeat_count(t, 1);
}

// Control4 announcement: a top banner (accent bar) with the message text,
// auto-dismissed after a few seconds. The chime is played separately (audio.c).
void ui_announce(const char *text)
{
    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(ov);
    lv_obj_set_width(ov, LV_PCT(100));
    lv_obj_set_height(ov, LV_SIZE_CONTENT);
    lv_obj_align(ov, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(ov, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(ov, 12, 0);
    lv_obj_set_flex_flow(ov, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ov, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(ov, 4, 0);

    lv_obj_t *hdr = lv_label_create(ov);
    lv_label_set_text(hdr, "ANNOUNCEMENT");
    lv_obj_set_style_text_color(hdr, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(hdr, F14, 0);

    if (text && text[0]) {
        lv_obj_t *body = lv_label_create(ov);
        lv_obj_set_width(body, LV_PCT(92));
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(body, text);
        lv_obj_set_style_text_color(body, lv_color_hex(C_TEXT), 0);
        lv_obj_set_style_text_font(body, F24, 0);
    }

    // Reuse identify_done: deletes the overlay + its timer after the delay.
    lv_timer_t *t = lv_timer_create(identify_done, 6000, ov);
    lv_timer_set_repeat_count(t, 1);
}

// ── Intercom call screen ─────────────────────────────────────────────────────
// Full-screen overlay shown for the duration of a call. Today: caller name +
// state + Answer/Decline/End. Structured so a video region (door camera) and a
// configurable action-button row (unlock / open gate) can slot into s_callBody
// later — gated on capability, like the camera flags.
static lv_obj_t *s_callState;   // "Incoming Call" / "Connected" / ...
static lv_obj_t *s_callPeer;    // caller name / AOR
static lv_obj_t *s_callBtns;    // action row (Answer/Decline or End)
static lv_obj_t *s_callIcon;    // door vs generic-intercom mark
static lv_obj_t *s_callDur;     // mm:ss once connected
static lv_timer_t *s_callTimer;
static uint32_t  s_callStart;

// Is this peer one of our door stations? The call screen only gets a name, but
// the roster knows the kind -- so a door call can show a door instead of the
// generic intercom mark.
static const intercom_target_t *call_peer_ep(const char *peer)
{
    if (!peer || !peer[0]) return NULL;
    for (int i = 0; i < s_nEps; i++)
        if (!strcmp(s_eps[i].name, peer) || !strcmp(s_eps[i].user, peer))
            return &s_eps[i];
    return NULL;
}
static bool call_peer_is_door(const char *peer)
{
    const intercom_target_t *e = call_peer_ep(peer);
    return e && e->door;
}

static void callDurTick(lv_timer_t *t)
{
    (void)t;
    if (!s_callDur) return;
    uint32_t sec = (millis() - s_callStart) / 1000;
    char b[16];
    snprintf(b, sizeof(b), "%u:%02u", (unsigned)(sec / 60), (unsigned)(sec % 60));
    lv_label_set_text(s_callDur, b);
}

static void onCallAnswer(lv_event_t *e) { (void)e; sip_answer(); }
static void onCallHangup(lv_event_t *e) { (void)e; sip_hangup(); }

static char s_callRemote[48];   // peer of the live call, for the unlock target
static lv_obj_t *s_callMuteLbl;

// Mute flips the SAME switch the driver's MUTE_CALL sets, then reports it up so
// the proxy (and Navigator's own call UI) sees the change.
static void onCallMute(lv_event_t *e)
{
    (void)e;
    bool on = !sip_is_muted();
    sip_set_mute(on);
    net_call_mute(on);
    if (s_callMuteLbl) lv_label_set_text(s_callMuteLbl, on ? "Unmute" : "Mute");
}
// One of the door's own actions was tapped. The button carries the action INDEX;
// the id sent on the wire comes from the roster entry, so the driver gets back
// exactly the id it advertised.
static void onCallDoor(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const intercom_target_t *ep = call_peer_ep(s_callRemote);
    if (!ep || idx < 0 || idx >= ep->n_actions) return;
    net_call_door(s_callRemote, ep->actions[idx].id);
}

static lv_obj_t *callBtn(lv_obj_t *parent, const char *txt, uint32_t bg, lv_event_cb_t cb, void *ud)
{
    // Sized off the ui scale: a fixed 124x52 pill is a thumbnail on a 10" panel,
    // and answering a door station is the one thing here that has to be hittable
    // without looking.
    const float s = s_uiscale;
    const int bw = (int)(190 * s), bh = (int)(68 * s);
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, bw, bh);
    lv_obj_set_style_radius(b, bh / 2, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(l, F24, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

void ui_call(const char *event, const char *peer)
{
    if (!event) return;
    if (!strcmp(event, "ended")) {
        if (s_callTimer) { lv_timer_delete(s_callTimer); s_callTimer = NULL; }
        if (s_call) {
            lv_obj_delete(s_call);
            s_call = NULL; s_callBtns = NULL; s_callIcon = NULL; s_callDur = NULL;
        }
        return;
    }
    if (!s_call) {
        lv_obj_t *ov = lv_obj_create(lv_layer_top());
        s_call = ov;
        lv_obj_remove_style_all(ov);
        lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);   // no stray scrollbar
        lv_obj_set_size(ov, LV_PCT(100), LV_PCT(100));
        // The app gradient, not a flat charcoal: this is the same product as the
        // page behind it, and a black slab read like a different app entirely.
        const float s = s_uiscale;
        lv_obj_set_style_bg_color(ov, lv_color_hex(HOME_BG_TOP), 0);
        lv_obj_set_style_bg_grad_color(ov, lv_color_hex(HOME_BG_BOT), 0);
        lv_obj_set_style_bg_grad_dir(ov, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
        lv_obj_set_flex_flow(ov, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(ov, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(ov, (int)(14 * s), 0);

        // An IMAGE icon scaled to the panel. This was a 24px font glyph at every
        // size -- a speck on a 10" -- because the 34px icon font lacks the bell.
        // An avatar puck: a soft accent circle with the caller's mark inside. The
        // icon is capped at its source resolution (the assets are 64/96px) -- scaled
        // to a full 84*s it went visibly soft on the 10" -- and the circle carries
        // the size instead, so the screen still reads from across the room.
        const int puck = (int)(150 * s);
        int isz = (int)(84 * s); if (isz > 96) isz = 96;
        lv_obj_t *ring = lv_obj_create(ov);
        lv_obj_remove_style_all(ring);
        lv_obj_set_size(ring, puck, puck);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(ring, lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_20, 0);

        lv_obj_t *icon = lv_image_create(ring);
        lv_obj_set_size(icon, isz, isz);
        lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
        lv_obj_set_style_image_recolor(icon, lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
        lv_obj_center(icon);
        s_callIcon = icon;

        s_callState = mkLabel(ov, F24, C_SUBTLE, 0, false);
        s_callPeer  = mkLabel(ov, F32, C_TEXT, LV_PCT(90), false);
        s_callDur   = mkLabel(ov, F24, C_SUBTLE, 0, false);
        lv_label_set_text(s_callDur, "");
        lv_obj_set_style_text_align(s_callPeer, LV_TEXT_ALIGN_CENTER, 0);

        s_callBtns = lv_obj_create(ov);
        lv_obj_remove_style_all(s_callBtns);
        lv_obj_clear_flag(s_callBtns, LV_OBJ_FLAG_SCROLLABLE);
        // WRAPS: a door with two of its own actions makes four buttons, which ran
        // straight off both edges of a 480px panel as a single row.
        lv_obj_set_size(s_callBtns, LV_PCT(92), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(s_callBtns, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(s_callBtns, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(s_callBtns, (int)(16 * s), 0);
        lv_obj_set_style_pad_row(s_callBtns, (int)(12 * s), 0);
        lv_obj_set_style_pad_top(s_callBtns, (int)(8 * s), 0);
    }

    const bool active = !strcmp(event, "active");
    const char *stateTxt = !strcmp(event, "incoming") ? "Incoming Call" :
                           !strcmp(event, "outgoing") ? "Calling\xE2\x80\xA6" : "Connected";
    lv_label_set_text(s_callState, stateTxt);
    lv_label_set_text(s_callPeer, (peer && peer[0]) ? peer : "Intercom");
    if (s_callIcon)
        lv_image_set_src(s_callIcon, call_peer_is_door(peer) ? ICON_DOOR : ICON_INTERCOM);

    // Count up only once connected, and only start the clock on the transition so
    // repeated "active" pushes don't keep resetting it.
    if (active) {
        if (!s_callTimer) {
            s_callStart = millis();
            s_callTimer = lv_timer_create(callDurTick, 1000, NULL);
            callDurTick(NULL);
        }
    } else {
        if (s_callTimer) { lv_timer_delete(s_callTimer); s_callTimer = NULL; }
        if (s_callDur) lv_label_set_text(s_callDur, "");
    }

    // Rebuild the action row for this state.
    lv_obj_clean(s_callBtns);
    s_callMuteLbl = NULL;
    snprintf(s_callRemote, sizeof(s_callRemote), "%s", (peer && peer[0]) ? peer : "");
    const intercom_target_t *ep = call_peer_ep(peer);
    if (!strcmp(event, "incoming")) {
        callBtn(s_callBtns, "Answer",  C_GREEN, onCallAnswer, NULL);
        callBtn(s_callBtns, "Decline", C_RED,   onCallHangup, NULL);
    } else {
        callBtn(s_callBtns, "End", C_RED, onCallHangup, NULL);
        if (active) {
            // Mute only makes sense once there is audio to mute. Starts from the
            // live state, so a driver-side MUTE_CALL is reflected here too.
            lv_obj_t *m = callBtn(s_callBtns, sip_is_muted() ? "Unmute" : "Mute",
                                  C_BTN, onCallMute, NULL);
            s_callMuteLbl = lv_obj_get_child(m, 0);
        }
    }
    // The endpoint's own door actions (0..2 -- typically door + gate), offered on
    // an INCOMING call too: letting someone in without answering first is the
    // common case at a door station. Labels come from the driver; the panel does
    // not invent them or guess which targets have them.
    if (ep) {
        for (int i = 0; i < ep->n_actions; i++) {
            callBtn(s_callBtns, ep->actions[i].label, C_ACCENT, onCallDoor, (void *)(intptr_t)i);
        }
    }

    // Wake the display for the call (and keep it lit — see ui_tick_screensaver).
    bsp_set_backlight(g_settings.brightness);
    lv_display_trigger_activity(NULL);
}

static lv_obj_t *s_setup;
static char s_setup_ap[24];
static char s_setup_pass[16];    // WPA2 PSK of the setup AP (unique per device, carried in the QR)
static char s_setup_pop[16];
static bool s_setup_has_pop;     // board offers BLE provisioning (pop != NULL)
static bool s_setup_ble;         // current mode: false = camera/Wi-Fi (default), true = app/BLE
static lv_display_rotation_t s_setup_prev_rot;   // display rotation to restore when setup closes

// The NuVoxel logo, decoded once from the embedded splash.png into an ARGB8888
// descriptor kept in PSRAM (LVGL renders it directly). Cached across rebuilds.
static lv_image_dsc_t s_setup_logo;
static void setup_logo_init(void)
{
    if (s_setup_logo.data) return;
    // The transparent NuVoxel logo (logo.png) — NOT splash.png, which has a baked
    // opaque white background and would render as a white box on the dark screen.
    extern const uint8_t logo_png_start[] asm("_binary_logo_png_start");
    extern const uint8_t logo_png_end[]   asm("_binary_logo_png_end");
    unsigned char *rgba = NULL; unsigned iw = 0, ih = 0;
    if (lodepng_decode32(&rgba, &iw, &ih, logo_png_start,
                         (size_t)(logo_png_end - logo_png_start)) != 0 || !rgba) {
        if (rgba) free(rgba);
        return;
    }
    // lodepng gives R,G,B,A; LVGL ARGB8888 stores B,G,R,A — swap R<->B.
    for (size_t i = 0; i < (size_t)iw * ih; i++) {
        unsigned char t = rgba[i*4]; rgba[i*4] = rgba[i*4+2]; rgba[i*4+2] = t;
    }
    size_t sz = (size_t)iw * ih * 4;
    uint8_t *ps = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);   // persist in PSRAM
    if (!ps) { free(rgba); return; }
    memcpy(ps, rgba, sz); free(rgba);
    s_setup_logo.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_setup_logo.header.cf     = LV_COLOR_FORMAT_ARGB8888;
    s_setup_logo.header.w      = iw;
    s_setup_logo.header.h      = ih;
    s_setup_logo.header.stride = iw * 4;
    s_setup_logo.header.flags  = 0;
    s_setup_logo.data_size     = (uint32_t)sz;
    s_setup_logo.data          = ps;
}

// Height of the actual ink in the wordmark art, in SOURCE pixels. The png is a
// 480x384 canvas that is ~85% transparent vertical padding; only ~72px carries the
// wordmark. Layout boxes must be sized from this, never from header.h.
#define LOGO_INK_H 72

// NuVoxel logo for the Settings page. Image source differs by platform: the
// ESP boards reuse the embedded logo.png (decoded into s_setup_logo above); the T3
// loads it from the rootfs via LVGL's POSIX FS. Scaled to a small ~30px banner.
static void settings_add_logo(lv_obj_t *parent)
{
    lv_obj_t *lg = lv_image_create(parent);
    // The logo is a WIDE wordmark (480x59 art in a 480x384 frame — 85% is transparent
    // vertical padding), so scale by WIDTH, not height, or the wordmark shrinks to
    // nothing. Target ~36% of screen width.
    int Wd = lv_display_get_horizontal_resolution(NULL);
    int tw = Wd * 36 / 100;
    int srcw = 480;   // source art width (both platforms ship the same 480x384 png)
#ifdef ESP_PLATFORM
    setup_logo_init();
    if (!s_setup_logo.data) { lv_obj_delete(lg); return; }
    lv_image_set_src(lg, &s_setup_logo);
    if (s_setup_logo.header.w) srcw = (int)s_setup_logo.header.w;
    lv_image_set_scale(lg, tw * 256 / srcw);
#else
    lv_image_set_src(lg, "A:/usr/share/nuvoxel-logo.png");   // LVGL FS POSIX (drive 'A')
    lv_image_set_scale(lg, tw * 256 / srcw);                 // source logo is 480 wide
#endif
    // lv_image_set_scale() changes only how the art is DRAWN — the object's layout
    // box stays the full source size (480x384). Left unset that box is wider than
    // the padded content area (horizontal scrollbar) and ~384px tall (vertical
    // scrollbar), for a wordmark that is only ~59px of real ink; the rest of the
    // canvas is transparent padding. So size the box to the SCALED wordmark and
    // centre the art inside it — same fix setup_build() already applies.
    lv_image_set_inner_align(lg, LV_IMAGE_ALIGN_CENTER);
    lv_obj_set_size(lg, tw, LOGO_INK_H * tw / srcw);
    lv_obj_set_style_image_opa(lg, LV_OPA_80, 0);

    // "Buy me a coffee" QR under the wordmark — support the project. Generated
    // from the URL at runtime (lv_qrcode), so there is no image asset to embed.
    static const char BMC_URL[] = "https://www.buymeacoffee.com/nuvoxel";
    int qsz = (int)(84 * s_uiscale);
    if (qsz < 72) qsz = 72;
    lv_obj_t *qr = lv_qrcode_create(parent);
    lv_qrcode_set_size(qr, qsz);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, BMC_URL, strlen(BMC_URL));
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);   // quiet zone so it scans
    lv_obj_set_style_border_width(qr, (int)(4 * s_uiscale), 0);
    lv_obj_t *cap = lv_label_create(parent);
    lv_label_set_text(cap, "Buy me a coffee");
    lv_obj_set_style_text_font(cap, F16, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(C_SUBTLE), 0);
}

static void setup_toggle_cb(lv_event_t *e);

// (Re)build the setup screen contents for the current mode into s_setup.
static void setup_build(void)
{
    lv_obj_clean(s_setup);

    // Size the screen to the panel: the big DSI panels (ws43, 480 wide) get the
    // logo + large type; the little 2.8" s3 (240x320 portrait) uses compact type
    // and a smaller QR.
    int Wd = lv_display_get_horizontal_resolution(NULL);
    bool big = Wd >= 400;
    const lv_font_t *f_title = big ? F32 : F24;
    const lv_font_t *f_hint  = big ? F24 : F16;
    int qr_sz = big ? 168 : 108;

    // NuVoxel wordmark up top: white recolor
    // at low opacity so the blue wordmark lifts off the blue gradient, and the
    // layout box sized to the ACTUAL wordmark height (the art is a 480x384 canvas
    // that is ~85% transparent vertical padding), so it doesn't eat the space the
    // QR + credentials need — the mistake that made the small panel skip it before.
    setup_logo_init();
    if (s_setup_logo.data) {
        int lw = big ? (Wd * 52 / 100) : (Wd * 66 / 100);   // wider fraction on the tiny panel
        uint16_t lsc = (uint16_t)(lw * 256 / (int)s_setup_logo.header.w);
        lv_obj_t *lg = lv_image_create(s_setup);
        lv_image_set_src(lg, &s_setup_logo);
        lv_image_set_scale(lg, lsc);
        lv_image_set_inner_align(lg, LV_IMAGE_ALIGN_CENTER);
        lv_obj_set_size(lg, (int)s_setup_logo.header.w * lsc / 256, 72 * lsc / 256);
        // Solid white wordmark: the blue art
        // over the blue gradient washes out at low opacity, and this screen is a
        // deliberate brand moment (it matches the white wordmark on the captive
        // portal the QR leads to). Recolor keeps the art's alpha, so only the
        // letterforms turn white.
        lv_obj_set_style_image_recolor(lg, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_image_recolor_opa(lg, LV_OPA_COVER, 0);
    }

    lv_obj_t *title = lv_label_create(s_setup);
    lv_label_set_text(title, s_setup_ble ? "App Setup" : "Wi-Fi Setup");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, f_title, 0);

    lv_obj_t *qr = lv_qrcode_create(s_setup);
    lv_qrcode_set_size(qr, qr_sz);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    // App/BLE mode → the ESP provisioning payload (scanned INSIDE the ESP BLE
    // Provisioning app). Camera/Wi-Fi mode → a plain Wi-Fi join code any phone
    // camera reads, which drops the user onto the softAP → captive portal.
    char payload[160];
    if (s_setup_ble && s_setup_has_pop) {
        snprintf(payload, sizeof(payload),
                 "{\"ver\":\"v1\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"ble\"}",
                 s_setup_ap, s_setup_pop);
    } else {
        // WPA2 join code — the phone auto-fills the (unique, per-device) password
        // from the QR, so the user never types it. A secured AP also makes iOS's
        // captive-portal auto-popup far more reliable than an open one.
        snprintf(payload, sizeof(payload), "WIFI:S:%s;T:WPA;P:%s;;", s_setup_ap, s_setup_pass);
    }
    lv_qrcode_update(qr, payload, strlen(payload));
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 6, 0);

    lv_obj_t *hint = lv_label_create(s_setup);
    if (s_setup_ble) {
        lv_label_set_text(hint, "Scan in the ESP BLE\nProvisioning app");
    } else {
        // SSID + password each on their own line so the (12-char) password never
        // gets clipped on the narrow 240px panel.
        lv_label_set_text_fmt(hint,
            "Scan to set up — or join:\n%s\npw  %s", s_setup_ap, s_setup_pass);
    }
    // Constrain width + wrap so a long line wraps instead of running off-screen.
    lv_obj_set_width(hint, LV_PCT(94));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(hint, f_hint, 0);

    // Toggle between the two setup methods — only when the board offers BLE prov.
    if (s_setup_has_pop) {
        lv_obj_t *btn = lv_button_create(s_setup);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1a2530), 0);
        lv_obj_set_style_pad_hor(btn, big ? 22 : 14, 0);
        lv_obj_set_style_pad_ver(btn, big ? 14 : 8, 0);
        lv_obj_add_event_cb(btn, setup_toggle_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *bl = lv_label_create(btn);
        lv_label_set_text(bl, s_setup_ble ? "Use camera / browser instead"
                                          : "Set up with the app");
        lv_obj_set_style_text_font(bl, f_hint, 0);
        lv_obj_center(bl);
    }
}

static void setup_toggle_cb(lv_event_t *e)
{
    (void)e;
    s_setup_ble = !s_setup_ble;
    setup_build();   // just swaps the QR/hint; BLE + softAP run concurrently already
}

void ui_show_setup(const char *ap_name, const char *ap_pass, const char *pop)
{
    if (s_setup) return;
    strncpy(s_setup_ap, ap_name ? ap_name : "", sizeof(s_setup_ap) - 1);
    strncpy(s_setup_pass, ap_pass ? ap_pass : "", sizeof(s_setup_pass) - 1);
    s_setup_has_pop = (pop != NULL);
    if (pop) strncpy(s_setup_pop, pop, sizeof(s_setup_pop) - 1);
    s_setup_ble = false;   // default to the universal camera/Wi-Fi path

    // The setup form is a portrait-shaped column — force the display to its native
    // portrait rotation while it's shown, even if the device is configured for
    // landscape (the main UI keeps that; we restore it in ui_hide_setup). Native-
    // landscape panels are already at ROTATION_0, so this is a no-op there.
    lv_display_t *disp = lv_display_get_default();
    s_setup_prev_rot = lv_display_get_rotation(disp);
    if (s_setup_prev_rot != LV_DISPLAY_ROTATION_0)
        lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_0);

    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    s_setup = ov;
    lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov, LV_PCT(100), LV_PCT(100));
    // The NuVoxel blue gradient (indigo -> blue, vertical), matching the home
    // screen so Wi-Fi setup reads as part of the same UI. HOME_BG_* is the X4/HA
    // theme's home gradient; ui_apply_theme() has already picked the theme.
    lv_obj_set_style_bg_color(ov, lv_color_hex(HOME_BG_TOP), 0);
    lv_obj_set_style_bg_grad_color(ov, lv_color_hex(HOME_BG_BOT), 0);
    lv_obj_set_style_bg_grad_dir(ov, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(ov, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ov, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(ov, 10, 0);
    setup_build();
}
void ui_hide_setup(void)
{
    if (s_setup) { lv_obj_delete(s_setup); s_setup = NULL; }
    // Restore the configured orientation the main UI runs at (portrait setup was
    // forced above).
    lv_display_t *disp = lv_display_get_default();
    if (disp && lv_display_get_rotation(disp) != s_setup_prev_rot)
        lv_display_set_rotation(disp, s_setup_prev_rot);
}
