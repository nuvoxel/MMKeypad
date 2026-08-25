#include "net.h"
#include "config.h"
#include "board.h"
#if MMK_HAS_AUDIO
#include "sip.h"      // intercom control rides the :6700 link (audio boards only)
#include "audio.h"
#endif
#ifdef PIN_RGB_LED
#include "halo.h"
#endif
#include "esp_netif.h"

#include <string.h>
#include <stdio.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"     // inet_ntop for the peer-pinning log
#include "esp_timer.h"     // esp_timer_get_time: peer-pin staleness
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_mac.h"
#if MMK_NET_WIFI
#include "esp_wifi.h"     // mmk_read_mac: remote-radio MAC on P4 (C6 co-processor)
#endif
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "device.h"
#include "cJSON.h"
#include <stdlib.h>

static const char *TAG = "net";

static net_callbacks_t s_cb;
static int s_conn = -1;                  // current client socket (-1 = none)
static SemaphoreHandle_t s_tx_mtx;       // guards s_conn for sends across tasks
static int  s_driver_proto = 0;          // driver's protocol version from `state` (0 = unknown)
static char s_peer_ip[16] = {0};         // the connected driver/Director's IP
static char s_last_room[48] = {0};       // last room the driver reported (for the platform report)
static char s_mac[18];

// ── send ────────────────────────────────────────────────────────────────────
void net_send_line(const char *json)
{
    if (!json) return;
    xSemaphoreTake(s_tx_mtx, portMAX_DELAY);
    int fd = s_conn;
    if (fd >= 0) {
        // One write incl. the newline so the frame isn't split across segments.
        size_t jl = strlen(json);
        char *buf = malloc(jl + 1);
        if (buf) {
            memcpy(buf, json, jl);
            buf[jl] = '\n';
            send(fd, buf, jl + 1, 0);
            free(buf);
        }
    }
    xSemaphoreGive(s_tx_mtx);
}

static void send_obj(cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    if (s) { net_send_line(s); cJSON_free(s); }
    cJSON_Delete(o);
}

void net_cmd(const char *c)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "cmd");
    cJSON_AddStringToObject(o, "cmd", c);
    send_obj(o);
}
void net_set_volume(int level)
{
    if (level < 0) level = 0; else if (level > 100) level = 100;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "vol");
    cJSON_AddNumberToObject(o, "level", level);
    send_obj(o);
}
void net_step_volume(bool up)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "vol");
    cJSON_AddStringToObject(o, "dir", up ? "up" : "down");
    send_obj(o);
}
// Thumbnail decode outcome -> driver log. Diagnostic only; the driver logs it and
// takes no action, and an older driver ignores an unknown type by contract.
void net_art_debug(int slot, bool ok, int bytes, int w, int h)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "artdbg");
    cJSON_AddNumberToObject(o, "slot", slot);
    cJSON_AddBoolToObject(o, "ok", ok);
    cJSON_AddNumberToObject(o, "bytes", bytes);
    cJSON_AddNumberToObject(o, "w", w);
    cJSON_AddNumberToObject(o, "h", h);
    send_obj(o);
}

void net_toggle_mute(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "mute");
    send_obj(o);
}
// Mic mute state for the live call, so the driver can mirror it to the proxy
// (NOTIFY.Mute_Audio_Changed) and Navigator's own call UI stays in step.
void net_call_mute(bool on)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "callctl");
    cJSON_AddStringToObject(o, "action", "mute");
    cJSON_AddBoolToObject(o, "on", on);
    send_obj(o);
}
// Fire one of the door's own actions (its relays -- door, gate, ...). The Control4
// intercom proxy has no such command, so the driver raises a programming EVENT the
// installer binds to a relay, lock or macro: vendor-neutral, and no guessing at a
// command vocabulary. `action_id` is the id the driver gave us in `endpoints`.
void net_call_door(const char *remote, const char *action_id)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "callctl");
    cJSON_AddStringToObject(o, "action", "door");
    if (remote && remote[0]) cJSON_AddStringToObject(o, "remote", remote);
    if (action_id && action_id[0]) cJSON_AddStringToObject(o, "id", action_id);
    send_obj(o);
}
void net_select_source(const char *id)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "source");
    cJSON_AddStringToObject(o, "id", id);
    send_obj(o);
}
void net_send_button(int id)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "button");
    cJSON_AddNumberToObject(o, "id", id);
    send_obj(o);
}
void net_request_rooms(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "getrooms");
    send_obj(o);
}
void net_group_room(const char *id, bool join)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "grouproom");
    cJSON_AddStringToObject(o, "id", id ? id : "");
    cJSON_AddBoolToObject(o, "join", join ? 1 : 0);
    send_obj(o);
}
void net_request_favorites(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "getfavorites");   // room-level; no source id
    send_obj(o);
}
void net_play_favorite(const char *id)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "favorite");
    cJSON_AddStringToObject(o, "id", id ? id : "");
    send_obj(o);
}
void net_ping(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "ping");
    send_obj(o);
}

// ── Offline relay client ────────────────────────────────────────────────────
// A panel on an isolated network can be licensed (the driver fetches that for it)
// but could not do the two things it has to fetch ITSELF: its cloud check-in and
// the firmware image. These helpers borrow the Director's WAN through the driver.
// One request is in flight at a time -- both callers are slow background tasks and
// serialising them keeps the correlation trivial.
static SemaphoreHandle_t s_relay_mtx;      // one relay request at a time
static SemaphoreHandle_t s_relay_done;     // signalled by the RX task with a reply
static volatile int      s_relay_id;       // correlation id of the in-flight request
static char             *s_relay_body;     // response body (cloudresp)
static size_t            s_relay_body_cap;
static volatile int      s_relay_code;     // HTTP status, or 0 on transport failure
static uint8_t          *s_relay_bin;      // decoded bytes (fwdata)
static volatile int      s_relay_bin_len;
static volatile bool     s_relay_eof;

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Decode base64 into `out`; returns bytes written, or -1 if it would overflow.
static int b64_decode(const char *in, uint8_t *out, size_t cap)
{
    int acc = 0, bits = 0; size_t n = 0;
    for (const char *p = in; *p; p++) {
        if (*p == '=' || *p == '\n' || *p == '\r') continue;
        int v = b64_val(*p);
        if (v < 0) continue;
        acc = (acc << 6) | v; bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= cap) return -1;
            out[n++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    return (int)n;
}

static void send_hello(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "hello");
    cJSON_AddStringToObject(o, "mac", s_mac);
    cJSON_AddStringToObject(o, "fw", fw_version());
    // Advertise whether this device has the SIP intercom — the driver only offers its
    // "Install Intercom" action when the device actually supports it.
    cJSON_AddBoolToObject(o, "intercom", MMK_HAS_SIP ? 1 : 0);
    // Device identity, so the C4 driver (which has WAN via the Director) can fetch
    // this device's license / OTA from nuvoxel and push it back over this link —
    // the offline-keypad path. Sent only to the bound, trusted driver.
    cJSON_AddStringToObject(o, "hwid", device_hardware_id());
    cJSON_AddStringToObject(o, "secret", device_secret_hex());
    cJSON_AddStringToObject(o, "sku", device_sku_id());
    // Full device manifest — model, SoC/hardware ids, display, connectivity,
    // power, capabilities — so the driver adapts to *this* device (which glass,
    // what it can do) rather than hardcoding per-SKU assumptions.
    device_manifest_to_json(cJSON_AddObjectToObject(o, "manifest"));
    send_obj(o);
}

bool net_connected(void) { return s_conn >= 0; }
int  net_driver_proto(void) { return s_driver_proto; }
const char *net_peer_ip(void) { return s_peer_ip; }
const char *net_current_room(void) { return s_last_room; }

// Best-effort primary local IPv4 as a string ("" if offline). Portable across the
// ESP (lwip) and T3 (Linux) builds — a UDP socket "connected" to a dummy public
// address sends nothing but makes the stack pick the egress interface, whose local
// address getsockname() then returns. Used by the on-screen Settings/info page.
void net_get_ip(char *buf, size_t n)
{
    if (!buf || n == 0) return;
    buf[0] = 0;
    // Read the assigned IPv4 straight off the active netif (STA/ETH). The old
    // "UDP connect to 8.8.8.8 + getsockname" trick returned 0.0.0.0 when LWIP
    // didn't populate the local addr on a routeless UDP connect.
    esp_netif_t *nif = mmk_default_netif();
    esp_netif_ip_info_t ipi;
    if (nif && esp_netif_get_ip_info(nif, &ipi) == ESP_OK && ipi.ip.addr != 0) {
        esp_ip4addr_ntoa(&ipi.ip, buf, n);
    }
}

// ── parse helpers ───────────────────────────────────────────────────────────
static void get_str(const cJSON *o, const char *k, char *dst, size_t n)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    if (cJSON_IsString(v) && v->valuestring) {
        strncpy(dst, v->valuestring, n - 1);
        dst[n - 1] = 0;
    }
}
static int get_int(const cJSON *o, const char *k, int def)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valueint : def;
}
static bool get_bool(const cJSON *o, const char *k, bool def)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    return def;
}

static void handle_state(const cJSON *d)
{
    // Version check (review H2): the driver sends `proto`; warn once on a mismatch.
    // We keep operating — fields are additive and unknown ones are ignored.
    int proto = get_int(d, "proto", 0);
    if (proto) s_driver_proto = proto;
    // The driver reports its own version (driverVersion) for the settings page.
    const cJSON *dv = cJSON_GetObjectItem(d, "driverVersion");
    if (cJSON_IsString(dv) && dv->valuestring) device_set_driver_version(dv->valuestring);
    static bool proto_warned = false;   // NET_PROTO_VERSION now lives in net.h
    if (proto != 0 && proto != NET_PROTO_VERSION && !proto_warned) {
        proto_warned = true;
        ESP_LOGW(TAG, "driver proto v%d != firmware v%d (continuing)", proto, NET_PROTO_VERSION);
    }
    // Static (not on the stack): media_state_t is ~3 KB and the net task stack is
    // modest. Safe because only the net task calls this, sequentially, and the
    // on_state callback runs synchronously before we return.
    static media_state_t st;
    memset(&st, 0, sizeof(st));
    get_str(d, "room", st.room, sizeof(st.room));
    if (st.room[0]) snprintf(s_last_room, sizeof(s_last_room), "%s", st.room);
    st.power   = get_bool(d, "power", false);
    st.playing = get_bool(d, "playing", false);
    get_str(d, "mediaType", st.media_type, sizeof(st.media_type));
    get_str(d, "title",  st.title,  sizeof(st.title));
    get_str(d, "artist", st.artist, sizeof(st.artist));
    get_str(d, "album",  st.album,  sizeof(st.album));
    get_str(d, "artUrl", st.art_url, sizeof(st.art_url));
    const cJSON *src = cJSON_GetObjectItem(d, "source");
    if (cJSON_IsObject(src)) {
        get_str(src, "id",   st.source.id,   sizeof(st.source.id));
        get_str(src, "name", st.source.name, sizeof(st.source.name));
    }
    st.volume   = get_int(d, "volume", 0);
    st.muted    = get_bool(d, "muted", false);
    st.duration = get_int(d, "duration", 0);
    st.position = get_int(d, "position", 0);
    // Element visibility (default shown if the driver doesn't send the flag).
    st.show_title    = get_bool(d, "showTitle", true);
    st.show_artist   = get_bool(d, "showArtist", true);
    st.show_info     = get_bool(d, "showInfo", true);
    st.show_progress = get_bool(d, "showProgress", true);
    // Transport caps (default to full play/pause + skip if the driver omits them).
    st.can_pause       = get_bool(d, "canPause", true);
    st.can_stop        = get_bool(d, "canStop", false);
    st.can_next        = get_bool(d, "canNext", true);
    st.can_prev        = get_bool(d, "canPrev", true);
    st.can_thumbs_up   = get_bool(d, "canThumbsUp", false);
    st.can_thumbs_down = get_bool(d, "canThumbsDown", false);
    st.can_shuffle     = get_bool(d, "canShuffle", false);
    st.can_repeat      = get_bool(d, "canRepeat", false);
    st.shuffle_on      = get_bool(d, "shuffleOn", false);
    st.repeat_on       = get_bool(d, "repeatOn", false);

    const cJSON *arr = cJSON_GetObjectItem(d, "meta");
    ESP_LOGI(TAG, "state: meta present=%d isarray=%d",
             arr ? 1 : 0, cJSON_IsArray(arr) ? 1 : 0);   // DIAG: empty info panel
    if (cJSON_IsArray(arr)) {
        const cJSON *it;
        cJSON_ArrayForEach(it, arr) {
            if (st.n_meta >= NET_MAX_META) break;
            get_str(it, "label", st.meta[st.n_meta].id,   sizeof(st.meta[0].id));
            get_str(it, "value", st.meta[st.n_meta].name, sizeof(st.meta[0].name));
            st.n_meta++;
        }
        ESP_LOGI(TAG, "state: parsed %d meta rows", st.n_meta);   // DIAG
    }
    arr = cJSON_GetObjectItem(d, "buttons");
    if (cJSON_IsArray(arr)) {
        const cJSON *it;
        cJSON_ArrayForEach(it, arr) {
            if (st.n_buttons >= NET_MAX_BUTTONS) break;
            key_btn_t *b = &st.buttons[st.n_buttons];
            b->id = get_int(it, "id", 0);
            get_str(it, "label", b->label, sizeof(b->label));
            b->on = get_bool(it, "on", false);
            strcpy(b->color, "000000");
            get_str(it, "color", b->color, sizeof(b->color));
            get_str(it, "icon", b->icon, sizeof(b->icon));
            get_str(it, "offIcon", b->off_icon, sizeof(b->off_icon));
            st.n_buttons++;
        }
    }

    // Driver-pushed display prefs. Persist, but DO NOT reboot from the rx path
    // (review fix): signal the app, which applies live where possible.
    int rot = get_int(d, "rotation", -1);
    int lay = get_int(d, "layout", -1);
    int bg  = get_int(d, "bg", -1);
    int bri = get_int(d, "brightness", -1);   // active backlight 0..100
    int dsc = get_int(d, "dimSec", -1);        // idle dim timeout (sec; 0 = never)
    int dlv = get_int(d, "dimLevel", -1);      // idle backlight 0..100 (0 = screen off)
    bool changed = false;
    // Backlight prefs apply live via ui_tick_screensaver (no display rebuild needed).
    if (bri >= 0 && bri <= 100 && bri != g_settings.brightness) {
        g_settings.brightness = (uint8_t)bri; settings_save();
    }
    if (dsc >= 0 && dsc != g_settings.screensaver_sec) {
        g_settings.screensaver_sec = (uint16_t)dsc; settings_save();
    }
    if (dlv >= 0 && dlv <= 100 && dlv != g_settings.dim_brightness) {
        g_settings.dim_brightness = (uint8_t)dlv; settings_save();
    }
    if (rot >= 0 && rot <= 3 && rot != g_settings.orientation) {
        g_settings.orientation = (uint8_t)rot; settings_save(); changed = true;
    }
    if (lay >= 0 && lay <= 2 && lay != g_settings.layout) {
        g_settings.layout = (uint8_t)lay; settings_save(); changed = true;
    }
    if (bg >= 0 && bg <= 3 && bg != g_settings.bg_preset) {
        g_settings.bg_preset = (uint8_t)bg; settings_save(); changed = true;
    }
    if (changed && s_cb.on_display_change) s_cb.on_display_change();

    if (s_cb.on_state) s_cb.on_state(&st);
}

// ── OTA ───────────────────────────────────────────────────────────────────
// Pull + flash a firmware image from a URL (driver "Check & Update Firmware").
// Streams {t:"ota",status,msg,pct} back so the driver can show progress, and only
// re-flashes when the image version differs from the running one.
static void ota_status(const char *status, const char *msg, int pct)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "ota");
    cJSON_AddStringToObject(o, "status", status);
    if (msg) cJSON_AddStringToObject(o, "msg", msg);
    if (pct >= 0) cJSON_AddNumberToObject(o, "pct", pct);
    send_obj(o);
}


static void handle_line(const char *line)
{
    cJSON *d = cJSON_Parse(line);
    if (!d) return;   // ignore malformed
    const cJSON *t = cJSON_GetObjectItem(d, "t");
    const char *ts = (cJSON_IsString(t) && t->valuestring) ? t->valuestring : "";

    if (!strcmp(ts, "state")) {
        handle_state(d);
    } else if (!strcmp(ts, "identify")) {
        if (s_cb.on_identify) s_cb.on_identify();
    } else if (!strcmp(ts, "announce")) {
        const cJSON *tx = cJSON_GetObjectItem(d, "text");
        const cJSON *ch = cJSON_GetObjectItem(d, "chime");
        const char *text = (cJSON_IsString(tx) && tx->valuestring) ? tx->valuestring : "";
        bool chime = cJSON_IsBool(ch) ? cJSON_IsTrue(ch) : true;   // default chime
        if (s_cb.on_announce) s_cb.on_announce(text, chime);
    } else if (!strcmp(ts, "chime")) {
        // Door chime (intercom driver PLAY_DOOR_CHIME): play the proper DOORBELL (distinct
        // from the announce notification chime) + show the optional banner without an extra
        // chime. Default door label if no text given.
        const cJSON *tx = cJSON_GetObjectItem(d, "text");
        const char *text = (cJSON_IsString(tx) && tx->valuestring) ? tx->valuestring : "Door";
#if MMK_HAS_AUDIO
        audio_doorbell_async();
#endif
        if (s_cb.on_announce) s_cb.on_announce(text, false);       // banner only, no chime
    } else if (!strcmp(ts, "ping")) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "t", "pong");
        send_obj(p);
    } else if (!strcmp(ts, "cloudresp")) {
        if (get_int(d, "id", -1) == s_relay_id) {
            s_relay_code = get_int(d, "code", 0);
            const cJSON *b = cJSON_GetObjectItem(d, "body");
            if (s_relay_body && s_relay_body_cap) {
                s_relay_body[0] = 0;
                if (cJSON_IsString(b) && b->valuestring)
                    snprintf(s_relay_body, s_relay_body_cap, "%s", b->valuestring);
            }
            xSemaphoreGive(s_relay_done);
        }
    } else if (!strcmp(ts, "fwdata")) {
        if (get_int(d, "id", -1) == s_relay_id) {
            const cJSON *b = cJSON_GetObjectItem(d, "b64");
            s_relay_eof = cJSON_IsTrue(cJSON_GetObjectItem(d, "eof"));
            s_relay_bin_len = -1;
            if (cJSON_IsString(b) && b->valuestring && s_relay_bin)
                s_relay_bin_len = b64_decode(b->valuestring, s_relay_bin, 4096);
            xSemaphoreGive(s_relay_done);
        }
    } else if (!strcmp(ts, "reboot")) {
        // Driver "Reboot Keypad" programming command.
        ESP_LOGW(TAG, "reboot requested by driver");
        esp_restart();
    } else if (!strcmp(ts, "license")) {
        // C4 driver relayed a license token (fetched from nuvoxel on our behalf,
        // since this keypad may have no WAN). Verify + persist offline; report back.
        const cJSON *tok = cJSON_GetObjectItem(d, "token");
        bool ok = cJSON_IsString(tok) && tok->valuestring &&
                  device_apply_license(tok->valuestring) == 0;
        ESP_LOGI(TAG, "license via driver: %s", ok ? "applied" : "rejected");
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "t", "license");
        cJSON_AddStringToObject(p, "status", ok ? "ok" : "error");
        send_obj(p);
    } else if (!strcmp(ts, "ota")) {
        // The `url` override is GONE. It let whoever was on the socket hand us an
        // arbitrary image to flash: TLS proves only that the attacker owns their own
        // domain, and this device does not verify the image signature, so that was
        // unauthenticated remote code execution. The device is authoritative about
        // where it updates from -- device_ota_check_now() does an authenticated
        // check-in and verifies the sha256 the CLOUD reported for the build.
        if (cJSON_GetObjectItem(d, "url"))
            ESP_LOGW(TAG, "ignoring ota url from the link; using the authenticated cloud check");
        {
            ota_status("checking", "cloud", -1);
            device_ota_check_now();
        }
    } else if (!strcmp(ts, "sip") || !strcmp(ts, "call") || !strcmp(ts, "callcfg")) {
        // Phase-3 intercom control is multiplexed onto this :6700 link (no second
        // TCP channel). sip.c provisions/registers and bridges call control; it
        // sends `sipstate`/`callstate` back via net_send_line(). No-op if the SIP
        // module never initialised (audio not ready at boot).
#if MMK_HAS_AUDIO
        sip_handle(d);
#endif
    } else if (!strcmp(ts, "doorstations")) {
        // Driver-pushed set of door-station SIP usernames (from Control4's isDoorStation
        // capability). Build a "|user1|user2|…|" set and hand it to sip.c so an incoming
        // call from a door station rings the doorbell chime. See PROTOCOL.md.
#if MMK_HAS_AUDIO
        const cJSON *arr = cJSON_GetObjectItem(d, "users");
        // static (not on the net-task stack — a 512B local overflows it); the net task
        // handles messages sequentially and sip_set_doorstations() copies it immediately.
        static char set[512];
        size_t len = 0;
        if (cJSON_IsArray(arr)) {
            set[len++] = '|';
            const cJSON *it;
            cJSON_ArrayForEach(it, arr) {
                if (cJSON_IsString(it) && it->valuestring) {
                    int n = snprintf(set + len, sizeof(set) - len, "%s|", it->valuestring);
                    if (n > 0 && (size_t)n < sizeof(set) - len) len += (size_t)n;
                    else break;   // out of room; keep what fits
                }
            }
        }
        set[len] = '\0';
        sip_set_doorstations(len > 1 ? set : NULL);
#endif
    } else if (!strcmp(ts, "endpoints")) {
        // Driver-pushed callable intercom targets (groups + endpoints) for the on-screen
        // Intercom picker. See PROTOCOL.md. (Handled by the UI on display boards.)
        const cJSON *arr = cJSON_GetObjectItem(d, "list");
        static intercom_target_t eps[NET_MAX_ENDPOINTS];
        int n = 0;
        if (cJSON_IsArray(arr)) {
            const cJSON *it;
            cJSON_ArrayForEach(it, arr) {
                if (n >= NET_MAX_ENDPOINTS) break;
                get_str(it, "name", eps[n].name, sizeof(eps[n].name));
                get_str(it, "user", eps[n].user, sizeof(eps[n].user));
                eps[n].door   = get_bool(it, "door", false);
                eps[n].mobile = get_bool(it, "mobile", false);
                // Door actions as configured on THAT endpoint (0..2).
                eps[n].n_actions = 0;
                const cJSON *acts = cJSON_GetObjectItem(it, "actions");
                if (cJSON_IsArray(acts)) {
                    const cJSON *a;
                    cJSON_ArrayForEach(a, acts) {
                        if (eps[n].n_actions >= NET_MAX_DOOR_ACTIONS) break;
                        door_action_t *da = &eps[n].actions[eps[n].n_actions];
                        get_str(a, "id",    da->id,    sizeof(da->id));
                        get_str(a, "label", da->label, sizeof(da->label));
                        if (da->label[0]) eps[n].n_actions++;   // unlabelled = unrenderable
                    }
                }
                eps[n].group = get_bool(it, "group", false);
                n++;
            }
        }
        if (s_cb.on_endpoints) s_cb.on_endpoints(eps, n);
    } else if (!strcmp(ts, "rooms")) {
        // Driver reply to `getrooms`: the multiroom add-rooms list (PROTOCOL.md).
        const cJSON *arr = cJSON_GetObjectItem(d, "list");
        static room_t rooms[NET_MAX_ROOMS];
        int n = 0;
        if (cJSON_IsArray(arr)) {
            const cJSON *it;
            cJSON_ArrayForEach(it, arr) {
                if (n >= NET_MAX_ROOMS) break;
                get_str(it, "id", rooms[n].id, sizeof(rooms[n].id));
                get_str(it, "name", rooms[n].name, sizeof(rooms[n].name));
                rooms[n].grouped = get_bool(it, "grouped", false);
                rooms[n].playing = get_bool(it, "playing", false);
                rooms[n].active  = get_bool(it, "active", false);
                n++;
            }
        }
        if (s_cb.on_rooms) s_cb.on_rooms(rooms, n);
    } else if (!strcmp(ts, "favorites")) {
        // Driver reply to `getfavorites`: this room's navigator favorite tiles (PROTOCOL.md).
        // Each entry: id / title / image (-> art_url) / kind (stream|broadcast).
        const cJSON *arr = cJSON_GetObjectItem(d, "list");
        static favorite_t favs[NET_MAX_FAVORITES];
        int n = 0;
        if (cJSON_IsArray(arr)) {
            const cJSON *it;
            cJSON_ArrayForEach(it, arr) {
                if (n >= NET_MAX_FAVORITES) break;
                get_str(it, "id",    favs[n].id,      sizeof(favs[n].id));
                get_str(it, "title", favs[n].title,   sizeof(favs[n].title));
                get_str(it, "image", favs[n].art_url, sizeof(favs[n].art_url));
                get_str(it, "kind",  favs[n].kind,    sizeof(favs[n].kind));
                n++;
            }
        }
        if (s_cb.on_favorites) s_cb.on_favorites(favs, n);
    } else if (!strcmp(ts, "sipvol")) {
        // Intercom proxy SET_SPEAKER_VOLUME / SET_MICROPHONE_GAIN / SET_RINGER_VOLUME
        // relayed by the driver (all 0..100). Speaker -> DAC %, ringer -> chime %,
        // mic % -> ES8311 ADC gain in dB (0..100 mapped to 0..40 dB).
#if MMK_HAS_AUDIO
        const cJSON *sp = cJSON_GetObjectItem(d, "speaker");
        const cJSON *mc = cJSON_GetObjectItem(d, "mic");
        const cJSON *rg = cJSON_GetObjectItem(d, "ringer");
        // The intercom proxy pushes speaker=0 / ringer=0 on CONNECT, while idle --
        // observed on hardware. Applied blindly that silently zeroed the panel's own
        // sound: the chime went mute (the "toggle Mute off and on to get audio back"
        // workaround was just re-applying the saved setting) and the selftest tone and
        // loopback went silent. A zero from SIP is only meaningful during a call; when
        // idle the user's Sound > Volume setting wins.
        bool call = audio_in_call();
        // Speaker: a 0 is meaningful only for the duration of a call. The panel's
        // own level is restored by audio_call_end(), so a call can no longer
        // leave the speaker silenced for good.
        if (cJSON_IsNumber(sp) && (call || sp->valueint > 0))
            audio_set_volume((uint8_t)sp->valueint);
        // Ringer: never 0, not even during a call. A ringer level is not a
        // call-time property, and the proxy pushes 0 freely -- applying it left
        // the panel unable to ring until somebody moved a slider in Composer.
        // Silencing the ringer is what do-not-disturb is for.
        if (cJSON_IsNumber(rg) && rg->valueint > 0)
            audio_set_ringer_volume((uint8_t)rg->valueint);
        int micpct = -1;
        if (cJSON_IsNumber(mc)) {
            micpct = mc->valueint < 0 ? 0 : (mc->valueint > 100 ? 100 : mc->valueint);
            // Same reasoning as the ringer: a 0 gain here is a dead microphone,
            // and the proxy sends it while idle. Mute is a separate control.
            if (micpct > 0 || call) audio_set_mic_gain((float)micpct * 0.4f);
        }
        ESP_LOGI(TAG, "sipvol: speaker=%d ringer=%d mic=%d",   // DIAG: verify slider chain
                 cJSON_IsNumber(sp) ? sp->valueint : -1,
                 cJSON_IsNumber(rg) ? rg->valueint : -1, micpct);
#endif
    } else if (!strcmp(ts, "halo")) {
        // Driver-configured halo RGB LED: idle color (r,g,b), pulse/ring color
        // (pr,pg,pb), brightness 0..100. Any subset may be present.
#ifdef PIN_RGB_LED
        const cJSON *r = cJSON_GetObjectItem(d, "r");
        const cJSON *g = cJSON_GetObjectItem(d, "g");
        const cJSON *b = cJSON_GetObjectItem(d, "b");
        if (cJSON_IsNumber(r) && cJSON_IsNumber(g) && cJSON_IsNumber(b))
            halo_set_color(r->valueint, g->valueint, b->valueint);
        const cJSON *pr = cJSON_GetObjectItem(d, "pr");
        const cJSON *pg = cJSON_GetObjectItem(d, "pg");
        const cJSON *pb = cJSON_GetObjectItem(d, "pb");
        if (cJSON_IsNumber(pr) && cJSON_IsNumber(pg) && cJSON_IsNumber(pb))
            halo_set_pulse_color(pr->valueint, pg->valueint, pb->valueint);
        const cJSON *br = cJSON_GetObjectItem(d, "bright");
        if (cJSON_IsNumber(br)) halo_set_brightness(br->valueint);
#endif
    } else if (!strcmp(ts, "audiotest")) {
        // Bring-up/field diagnostic: run the audio self-test (chime + tone, mic
        // capture w/ level report, mic->speaker loopback). See audio_selftest_async.
#if MMK_HAS_AUDIO
        audio_selftest_async();
#endif
    }
    // unknown types: ignore (forward-compat)
    cJSON_Delete(d);
}

// ── server task ─────────────────────────────────────────────────────────────
static void serve_client(int c)
{
    int one = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    // #1: bound send() so a half-open/wedged link (Wi-Fi flaked mid-session) can't block
    // the sender forever. net_send_line()'s send() is reachable from UI event handlers
    // that run while holding the LVGL lock — an unbounded block there freezes the whole
    // UI (render + touch). A timed-out send just drops that message.
    struct timeval snd_to = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &snd_to, sizeof(snd_to));

    xSemaphoreTake(s_tx_mtx, portMAX_DELAY);
    if (s_conn >= 0) close(s_conn);   // newest wins
    s_conn = c;
    xSemaphoreGive(s_tx_mtx);

    // Capture the Director/driver IP for the on-screen info page.
    struct sockaddr_in pa; socklen_t pl = sizeof(pa);
    if (getpeername(c, (struct sockaddr *)&pa, &pl) == 0) {
        uint32_t ip = ntohl(pa.sin_addr.s_addr);
        snprintf(s_peer_ip, sizeof(s_peer_ip), "%u.%u.%u.%u", (unsigned)(ip >> 24) & 0xFF,
                 (unsigned)(ip >> 16) & 0xFF, (unsigned)(ip >> 8) & 0xFF, (unsigned)ip & 0xFF);
    }

    send_hello();
    if (s_cb.on_connect) s_cb.on_connect();

    char rx[1024];
    // The driver's `state` frame carries the source list, the button list, the track-info
    // meta rows and every capability flag. At 2048 it outgrew this buffer, and the
    // overflow path DISCARDS THE WHOLE FRAME SILENTLY -- so the panel simply stopped
    // getting state: no now-playing, and an empty track-info panel, with nothing in any
    // log to say why. Small frames (sipvol/license/halo) kept arriving, which made it
    // look like the link was healthy. Heap-allocated: too big for this task's stack.
    const size_t LINE_CAP = 8192;
    char *line = malloc(LINE_CAP);
    if (!line) { ESP_LOGE(TAG, "no mem for rx line buffer"); close(c); return; }
    size_t ll = 0;
    bool overflow = false;   // discard until next '\n' rather than corrupt the next frame
    for (;;) {
        int n = recv(c, rx, sizeof(rx), 0);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            char ch = rx[i];
            if (ch == '\n') {
                if (overflow) ESP_LOGW(TAG, "dropped an oversized frame (>%u bytes)",
                                       (unsigned)(LINE_CAP - 1));   // never silent again
                if (!overflow && ll > 0) { line[ll] = 0; handle_line(line); }
                ll = 0; overflow = false;
            } else if (ch != '\r') {
                if (ll < LINE_CAP - 1) line[ll++] = ch;
                else overflow = true;
            }
        }
    }

    free(line);
    xSemaphoreTake(s_tx_mtx, portMAX_DELAY);
    if (s_conn == c) s_conn = -1;
    xSemaphoreGive(s_tx_mtx);
    s_driver_proto = 0; s_peer_ip[0] = 0;
    close(c);
    if (s_cb.on_disconnect) s_cb.on_disconnect();
}

// See the accept loop: the peer we are currently bound to, and when we last saw it.
// RAM-only, cleared by a reboot.
#define PEER_STICKY_MS (5u * 60u * 1000u)
static uint32_t s_pin_ip, s_pin_last_ms;

static void server_task(void *arg)
{
    int port = (int)(intptr_t)arg;
    for (;;) {
        int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (ls < 0) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        int one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in a = {
            .sin_family = AF_INET,
            .sin_addr.s_addr = htonl(INADDR_ANY),
            .sin_port = htons(port),
        };
        if (bind(ls, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(ls, 1) < 0) {
            ESP_LOGW(TAG, "bind/listen :%d failed, retry", port);
            close(ls);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        ESP_LOGI(TAG, "listening on :%d", port);
        for (;;) {
            struct sockaddr_in ra;
            socklen_t rl = sizeof(ra);
            int c = accept(ls, (struct sockaddr *)&ra, &rl);
            if (c < 0) break;   // listen socket died -> recreate
            // PEER PINNING. This socket is unauthenticated: whoever connects is
            // handed our identity in `hello` and can push SIP credentials, flip the
            // intercom to silent auto-answer, or reboot us. Until the link carries
            // real authentication, stick to the FIRST peer we serve and refuse the
            // rest -- that also stops an idle attacker connection from holding the
            // single-client accept loop and locking the real driver out.
            //
            // Deliberately NOT persisted, and it lapses after PEER_STICKY_MS of that
            // peer not appearing: a controller that moves to a new DHCP address, or a
            // pin set by whoever happened to connect first, must not be able to lock
            // a panel out permanently. A reboot always clears it.
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            uint32_t ip  = (uint32_t)ra.sin_addr.s_addr;
            if (s_pin_ip && ip != s_pin_ip && (now - s_pin_last_ms) < PEER_STICKY_MS) {
                char who[16];
                inet_ntop(AF_INET, &ra.sin_addr, who, sizeof(who));
                ESP_LOGW(TAG, "refusing connection from %s (bound to another peer)", who);
                close(c);
                continue;
            }
            s_pin_ip = ip;
            s_pin_last_ms = now;
            ESP_LOGI(TAG, "driver connected");
            serve_client(c);
            s_pin_last_ms = (uint32_t)(esp_timer_get_time() / 1000);
        }
        close(ls);
    }
}

// Ask the driver to POST one of our cloud endpoints on our behalf. Returns 0 on a
// completed exchange (check *status), <0 if the link is down or the driver never
// answered. Blocking: called from background tasks only.
int net_relay_post(const char *path, const char *body, char *resp, size_t cap, int *status)
{
    if (!s_relay_mtx || !net_connected()) return -1;
    xSemaphoreTake(s_relay_mtx, portMAX_DELAY);
    int rc = -1;
    s_relay_body = resp; s_relay_body_cap = cap; s_relay_code = 0;
    s_relay_id++;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "cloudreq");
    cJSON_AddNumberToObject(o, "id", s_relay_id);
    cJSON_AddStringToObject(o, "path", path);
    cJSON_AddStringToObject(o, "body", body ? body : "{}");
    send_obj(o);
    // 20s: the driver has to do a WAN round trip on a slow controller.
    if (xSemaphoreTake(s_relay_done, pdMS_TO_TICKS(20000)) == pdTRUE) {
        if (status) *status = s_relay_code;
        rc = 0;
    } else {
        ESP_LOGW(TAG, "relay: no cloudresp for %s", path);
    }
    s_relay_body = NULL; s_relay_body_cap = 0;
    xSemaphoreGive(s_relay_mtx);
    return rc;
}

// Pull one range of the firmware image through the driver. Returns bytes written
// (0 at EOF) or <0 on failure.
int net_relay_fetch(const char *url, size_t off, uint8_t *out, size_t cap, bool *eof)
{
    if (!s_relay_mtx || !net_connected()) return -1;
    xSemaphoreTake(s_relay_mtx, portMAX_DELAY);
    int rc = -1;
    s_relay_bin = out; s_relay_bin_len = -1; s_relay_eof = false;
    s_relay_id++;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "fwget");
    cJSON_AddNumberToObject(o, "id", s_relay_id);
    cJSON_AddStringToObject(o, "url", url);
    cJSON_AddNumberToObject(o, "off", (double)off);
    cJSON_AddNumberToObject(o, "len", (double)(cap > 4096 ? 4096 : cap));
    send_obj(o);
    if (xSemaphoreTake(s_relay_done, pdMS_TO_TICKS(20000)) == pdTRUE) {
        rc = s_relay_bin_len;
        if (eof) *eof = s_relay_eof;
    } else {
        ESP_LOGW(TAG, "relay: no fwdata at offset %u", (unsigned)off);
    }
    s_relay_bin = NULL;
    xSemaphoreGive(s_relay_mtx);
    return rc;
}

void net_start(uint16_t port, const net_callbacks_t *cb)
{
    s_cb = *cb;
    s_tx_mtx = xSemaphoreCreateMutex();
    s_relay_mtx = xSemaphoreCreateMutex();
    s_relay_done = xSemaphoreCreateBinary();
    uint8_t mac[6];
    mmk_read_mac(mac);
    snprintf(s_mac, sizeof(s_mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    // On audio boards the driver's `sip` message routes to sip_handle() → esp_rtc_service_init()
    // *inline on this task* (DNS + TCP connect + REGISTER build) — several KB of stack. 6 KB
    // overflowed the "net" task mid-registration and rebooted the panel, which the driver read
    // as connect/disconnect flapping (it re-pushes creds on every reconnect → boot-loop). Give
    // audio builds room; non-audio boards never touch SIP, so they keep the lean stack.
#if MMK_HAS_AUDIO
    const uint32_t net_stack = 16384;
#else
    const uint32_t net_stack = 6144;
#endif
    xTaskCreate(server_task, "net", net_stack, (void *)(intptr_t)port, 5, NULL);
}

// ── transport-agnostic helpers (see net.h) ──────────────────────────────────
esp_netif_t *mmk_default_netif(void)
{
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("ETH_DEF");
    if (!n) n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    return n;
}

void mmk_read_mac(uint8_t mac[6])
{
#if MMK_NET_ETH
    esp_read_mac(mac, ESP_MAC_ETH);
#elif MMK_NET_WIFI && CONFIG_IDF_TARGET_ESP32P4
    // WiFi MAC lives on the C6 co-processor (esp_wifi_remote); the local efuse
    // has none. Fall back to the base MAC if the remote radio isn't up yet.
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK)
        esp_read_mac(mac, ESP_MAC_BASE);
#elif MMK_NET_WIFI
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
#else
    esp_read_mac(mac, ESP_MAC_BASE);   // no transport yet (e.g. crowpanel pre-WiFi)
#endif
}
