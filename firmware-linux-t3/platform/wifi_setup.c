#define _GNU_SOURCE
#include "wifi_setup.h"
#include "wifi_linux.h"

#include <ifaddrs.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "lvgl.h"

// LVGL lock shared with the render loop (defined in main_linux.c). The scan and
// connect worker threads mutate LVGL objects, so they must hold it.
extern void ui_lock(void);
extern void ui_unlock(void);

// ── palette (NuVoxel dark, matches the boot splash 0x101418) ────────────────
#define C_BG      0x0E1216
#define C_PANEL   0x1A2029
#define C_PANEL2  0x232C37
#define C_ACCENT  0x4DA3FF
#define C_TEXT    0xE6EDF3
#define C_MUTED   0x8B98A5
#define C_OK      0x3FB950
#define C_ERR     0xF06A5B

#define MAX_NETS 24

static lv_obj_t  *g_scr;          // full-screen overlay root (NULL when hidden)
static lv_obj_t  *g_list;         // network list
static lv_obj_t  *g_status;       // status line under the title
static lv_obj_t  *g_rescan;       // rescan button
static lv_obj_t  *g_modal;        // password modal (NULL when hidden)
static wifi_net_t g_nets[MAX_NETS];
static int        g_net_count;
static char       g_sel_ssid[64];
static bool       g_busy;         // a scan or connect worker is in flight

// ── connectivity probe (eth0 or wlan0 holding an IPv4) ──────────────────────
static bool any_online(void) {
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) != 0) return false;
    bool up = false;
    for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (!p->ifa_name) continue;
        // Ignore loopback and the USB debug gadget (rndis0/usb0 carries a static
        // 10.55.0.x with no route to anywhere) -- neither is real connectivity,
        // and counting them makes an offline unit skip the WiFi picker.
        if (strcmp(p->ifa_name, "lo") == 0) continue;
        if (strncmp(p->ifa_name, "rndis", 5) == 0) continue;
        if (strncmp(p->ifa_name, "usb", 3) == 0) continue;
        struct sockaddr_in *s = (struct sockaddr_in *)p->ifa_addr;
        if (s->sin_addr.s_addr != 0) { up = true; break; }
    }
    freeifaddrs(ifa);
    return up;
}

// ── password modal ──────────────────────────────────────────────────────────
static void connect_selected(const char *psk);

static void modal_close(void) {
    if (g_modal) { lv_obj_delete(g_modal); g_modal = NULL; }
}

static void modal_cancel_cb(lv_event_t *e) { (void)e; modal_close(); }

static void modal_connect_cb(lv_event_t *e) {
    lv_obj_t *ta = lv_event_get_user_data(e);
    const char *psk = lv_textarea_get_text(ta);
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", psk ? psk : "");
    modal_close();
    connect_selected(buf);
}

static void kb_ready_cb(lv_event_t *e) { modal_connect_cb(e); }

static void open_password_modal(const char *ssid) {
    snprintf(g_sel_ssid, sizeof(g_sel_ssid), "%s", ssid);

    g_modal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_modal);
    lv_obj_set_size(g_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_modal, LV_OPA_70, 0);
    lv_obj_clear_flag(g_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(g_modal);
    lv_obj_set_size(card, 560, LV_SIZE_CONTENT);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 24, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text_fmt(t, "Password for \"%s\"", ssid);
    lv_obj_set_style_text_color(t, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);

    lv_obj_t *ta = lv_textarea_create(card);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, true);
    lv_textarea_set_placeholder_text(ta, "Network password");
    lv_obj_set_style_bg_color(ta, lv_color_hex(C_PANEL2), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(C_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_20, 0);

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);

    lv_obj_t *cancel = lv_button_create(row);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(C_PANEL2), 0);
    lv_obj_add_event_cb(cancel, modal_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, lv_color_hex(C_TEXT), 0);

    lv_obj_t *conn = lv_button_create(row);
    lv_obj_set_style_bg_color(conn, lv_color_hex(C_ACCENT), 0);
    lv_obj_add_event_cb(conn, modal_connect_cb, LV_EVENT_CLICKED, ta);
    lv_obj_t *cnl = lv_label_create(conn);
    lv_label_set_text(cnl, "Connect");
    lv_obj_set_style_text_color(cnl, lv_color_hex(0x08202E), 0);

    // On-screen keyboard bound to the field (Enter = connect). Bump the key font
    // (default is ~14px, too small on the physically larger 10" panel).
    lv_obj_t *kb = lv_keyboard_create(g_modal);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_set_size(kb, LV_PCT(100), LV_PCT(50));
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_28, 0);
    lv_obj_add_event_cb(kb, kb_ready_cb, LV_EVENT_READY, ta);
}

// ── scan ────────────────────────────────────────────────────────────────────
static void net_item_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= g_net_count) return;
    if (g_nets[idx].secured) open_password_modal(g_nets[idx].ssid);
    else                     { snprintf(g_sel_ssid, sizeof(g_sel_ssid), "%s", g_nets[idx].ssid);
                               connect_selected(""); }
}

static void populate_list(void) {
    lv_obj_clean(g_list);
    if (g_net_count == 0) {
        lv_obj_t *b = lv_list_add_text(g_list, "No networks found — tap Rescan");
        lv_obj_set_style_text_color(b, lv_color_hex(C_MUTED), 0);
        return;
    }
    for (int i = 0; i < g_net_count; i++) {
        // Secured nets get a trailing key-request marker (no lock glyph in the
        // built-in symbol font); tapping one opens the password prompt.
        char label[96];
        snprintf(label, sizeof(label), "%s%s", g_nets[i].ssid,
                 g_nets[i].secured ? "   " LV_SYMBOL_EYE_CLOSE : "");
        lv_obj_t *btn = lv_list_add_button(g_list, LV_SYMBOL_WIFI, label);
        lv_obj_set_style_bg_color(btn, lv_color_hex(C_PANEL), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(C_PANEL2), LV_STATE_PRESSED);
        lv_obj_set_style_text_color(btn, lv_color_hex(C_TEXT), 0);
        lv_obj_set_style_text_font(btn, &lv_font_montserrat_20, 0);
        lv_obj_add_event_cb(btn, net_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void set_status(const char *txt, uint32_t color) {
    if (!g_status) return;
    lv_label_set_text(g_status, txt);
    lv_obj_set_style_text_color(g_status, lv_color_hex(color), 0);
}

static void *scan_thread(void *arg) {
    (void)arg;
    wifi_net_t tmp[MAX_NETS];
    int n = wifi_scan(tmp, MAX_NETS);
    ui_lock();
    if (g_scr) { // still showing
        g_net_count = (n > 0) ? n : 0;
        if (n > 0) memcpy(g_nets, tmp, sizeof(wifi_net_t) * n);
        populate_list();
        set_status(n > 0 ? "Select your network" : "No networks found", C_MUTED);
        lv_obj_clear_state(g_rescan, LV_STATE_DISABLED);
    }
    g_busy = false;
    ui_unlock();
    return NULL;
}

static void start_scan(void) {
    if (g_busy) return;
    g_busy = true;
    set_status("Scanning\xE2\x80\xA6", C_ACCENT);
    lv_obj_add_state(g_rescan, LV_STATE_DISABLED);
    pthread_t t;
    if (pthread_create(&t, NULL, scan_thread, NULL) == 0) pthread_detach(t);
    else { g_busy = false; set_status("WiFi hardware unavailable", C_ERR); }
}

static void rescan_cb(lv_event_t *e) { (void)e; start_scan(); }

// ── connect ─────────────────────────────────────────────────────────────────
static char g_pending_psk[128];

static void *connect_thread(void *arg) {
    (void)arg;
    bool ok = wifi_connect(g_sel_ssid, g_pending_psk[0] ? g_pending_psk : NULL);
    ui_lock();
    if (g_scr) {
        if (ok) {
            set_status("Connected", C_OK);
            // Reveal the keypad UI: tear down the whole overlay after a beat.
            lv_obj_delete(g_scr);
            g_scr = g_list = g_status = g_rescan = NULL;
        } else {
            set_status("Couldn't connect — check the password and try again", C_ERR);
            lv_obj_clear_state(g_rescan, LV_STATE_DISABLED);
        }
    }
    g_busy = false;
    ui_unlock();
    return NULL;
}

static void connect_selected(const char *psk) {
    if (g_busy) return;
    snprintf(g_pending_psk, sizeof(g_pending_psk), "%s", psk ? psk : "");
    g_busy = true;
    char msg[128];
    snprintf(msg, sizeof(msg), "Connecting to \"%s\"\xE2\x80\xA6", g_sel_ssid);
    set_status(msg, C_ACCENT);
    lv_obj_add_state(g_rescan, LV_STATE_DISABLED);
    pthread_t t;
    if (pthread_create(&t, NULL, connect_thread, NULL) == 0) pthread_detach(t);
    else { g_busy = false; set_status("Connect failed to start", C_ERR); }
}

// ── screen ──────────────────────────────────────────────────────────────────
void wifi_setup_show(void) {
    if (g_scr) return; // already up

    g_scr = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_scr);
    lv_obj_set_size(g_scr, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(g_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(g_scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(g_scr, 48, 0);
    lv_obj_set_style_pad_row(g_scr, 8, 0);

    lv_obj_t *title = lv_label_create(g_scr);
    lv_label_set_text(title, "Connect to WiFi");
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    g_status = lv_label_create(g_scr);
    lv_obj_set_style_text_font(g_status, &lv_font_montserrat_16, 0);
    set_status("Scanning\xE2\x80\xA6", C_ACCENT);

    g_list = lv_list_create(g_scr);
    lv_obj_set_size(g_list, 720, 560);
    lv_obj_set_style_bg_color(g_list, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(g_list, 0, 0);
    lv_obj_set_style_pad_row(g_list, 6, 0);

    g_rescan = lv_button_create(g_scr);
    lv_obj_set_style_bg_color(g_rescan, lv_color_hex(C_PANEL2), 0);
    lv_obj_add_event_cb(g_rescan, rescan_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(g_rescan);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH "  Rescan");
    lv_obj_set_style_text_color(rl, lv_color_hex(C_TEXT), 0);

    start_scan();
}

// True if eth0 reports a live carrier (cable/PoE link up), independent of whether
// DHCP has finished. On a wired unit the lease can land tens of seconds after boot
// (slow PoE-switch negotiation — init logs "NO carrier after 8s" then leases
// later), so carrier is what tells us to WAIT rather than fall to the WiFi picker
// prematurely.
static bool eth_carrier(void) {
    FILE *f = fopen("/sys/class/net/eth0/carrier", "r");
    if (!f) return false;
    int c = fgetc(f);
    fclose(f);
    return c == '1';
}

// Tear down the picker overlay (reveals the claim/keypad UI underneath). Safe to
// call when it isn't up. Caller holds the UI lock.
static void wifi_setup_teardown(void) {
    if (g_scr) { lv_obj_delete(g_scr); g_scr = g_list = g_status = g_rescan = NULL; }
}

// The gate blocks (ensure_up + up to ~6s waiting for a saved net to associate),
// so it runs off the render thread; only the screen build takes the LVGL lock.
static void *gate_thread(void *arg) {
    (void)arg;
    // Support/override hook: `touch /data/wifi-setup` forces the picker up on the
    // next launch even when a network is present (e.g. to move the unit to a new
    // AP). Consumed once so it doesn't wedge the UI on every boot.
    if (access("/data/wifi-setup", F_OK) == 0) {
        remove("/data/wifi-setup");
        ui_lock(); wifi_setup_show(); ui_unlock();
        return NULL;
    }

    // Give a WIRED unit time to come online before concluding it needs WiFi. If
    // eth0 has a carrier we're on ethernet and the lease is just late — poll for
    // it (~45s) instead of flashing the WiFi picker at a unit that's about to be
    // online. A genuinely WiFi-only unit (no eth0 carrier) skips the wait.
    if (eth_carrier()) {
        for (int i = 0; i < 22; i++) {   // ~44s, 2s cadence
            if (any_online()) return NULL;
            sleep(2);
        }
    } else if (any_online()) {
        return NULL;
    }

    // Bring the stack up: wpa_supplicant auto-associates to a previously-saved
    // network from /data/wpa_supplicant.conf. Association alone gives no IP, so
    // wait for COMPLETED and pull a DHCP lease. If that gets us online, stay
    // silent; otherwise show the picker.
    if (wifi_ensure_up()) {
        wifi_await_dhcp(8);
        if (any_online()) return NULL;
    }
    ui_lock();
    wifi_setup_show();
    ui_unlock();

    // Keep watching with the picker up: if ethernet comes up late, or the user
    // plugs in PoE while looking at the WiFi screen, auto-dismiss and reveal the
    // claim screen rather than stranding them on WiFi. (The picker used to be
    // shown once and never re-evaluated, so plugging in ethernet did nothing —
    // the exact symptom on the wired 10" T3.)
    for (;;) {
        sleep(3);
        if (any_online()) {
            ui_lock(); wifi_setup_teardown(); ui_unlock();
            return NULL;
        }
    }
}

void wifi_setup_maybe_show(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, gate_thread, NULL) == 0) pthread_detach(t);
}
