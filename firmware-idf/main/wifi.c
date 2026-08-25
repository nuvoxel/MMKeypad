#include "wifi.h"
#include "device.h"   // device_variant_tag()
#include "board.h"
#include "ui.h"
#ifdef MMK_HAS_BLE_PROV
#include "prov.h"
#endif
#include "esp_lvgl_port.h"
#include <string.h>
#include <stdlib.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "wifi";

// Set to 1 to use a locally-administered STA MAC derived from the factory MAC, to dodge
// a controller-side block tied to the original MAC. Each unit stays unique. See wifi_start.
#ifndef MMK_WIFI_MAC_OVERRIDE
#define MMK_WIFI_MAC_OVERRIDE 1
#endif

static EventGroupHandle_t s_eg;
#define BIT_CONNECTED BIT0
static bool s_up = false;
/* "<variant tag>-<6 hex>" — the tag is up to 23 chars, so 24 was too small to
 * hold the longest legal name and the SSID would silently truncate. 33 = the
 * 802.11 SSID maximum (32) plus the terminator. */
static char s_ap[33];
static char s_ap_pop[7];   /* 6-hex MAC suffix of s_ap; the BLE provisioning PoP */
static char s_pass[16];   // WPA2 PSK of the setup AP — unique per device (full MAC hex)
static httpd_handle_t s_portal;
static esp_netif_t   *s_ap_netif;     // softAP netif (created once, reused across start/stop)
static volatile bool  s_dns_run;      // captive DNS task run flag (cleared to stop it)
// Suppress on_evt's STA auto-(re)connect while the softAP portal is up with no creds
// yet. Otherwise esp_wifi_connect() with an empty config fails → STA_DISCONNECTED →
// reconnect → a channel-hopping storm that STARVES the softAP beacon (the AP shows
// AP_START but never appears in a scan). Cleared when the user submits creds.
static volatile bool  s_hold_sta;
#ifdef MMK_HAS_BLE_PROV
// While BLE provisioning is running, the wifi_provisioning manager drives the STA
// connect itself (erases RAM creds, sets host creds on CRED_RECV, connects via its
// own timer, observes the result to raise CRED_SUCCESS/FAIL). Suppress on_evt's
// auto-(re)connect during that window so we don't fire competing esp_wifi_connect()
// calls that would confuse the manager's state machine. GOT_IP still sets
// BIT_CONNECTED regardless of path. Cleared once a connection is established.
static volatile bool s_prov_active;
#endif

static void on_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_hold_sta) return;      // portal up, no creds yet — don't storm the radio
#ifdef MMK_HAS_BLE_PROV
        if (s_prov_active) return;   // let the prov manager drive STA connect
#endif
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "STA disconnected from '%.*s' reason=%d rssi=%d",
                 d->ssid_len, (const char *)d->ssid, d->reason, d->rssi);
        s_up = false;
        if (s_hold_sta) return;      // portal up, no creds yet — don't retry-storm
#ifdef MMK_HAS_BLE_PROV
        if (s_prov_active) return;   // prov manager owns retry/CRED_FAIL handling
#endif
        esp_wifi_connect();   // keep retrying
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_up = true;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_eg, BIT_CONNECTED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        ESP_LOGW(TAG, "AP_START — softAP radio is beaconing");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STOP) {
        ESP_LOGW(TAG, "AP_STOP — softAP radio down");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *c = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "setup AP: client joined aid=%d " MACSTR, c->aid, MAC2STR(c->mac));
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *c = (wifi_event_ap_stadisconnected_t *)data;
        ESP_LOGI(TAG, "setup AP: client left aid=%d " MACSTR " reason=%d", c->aid, MAC2STR(c->mac), c->reason);
    }
}

static bool try_sta(const char *ssid, const char *pass, int wait_ms)
{
    // esp_wifi's ssid/password fields are fixed-size and need no NUL terminator —
    // bounded memcpy into the zeroed struct (also allows full 32-char SSIDs).
    wifi_config_t wc = {0};
    memcpy(wc.sta.ssid, ssid, strnlen(ssid, sizeof(wc.sta.ssid)));
    if (pass) memcpy(wc.sta.password, pass, strnlen(pass, sizeof(wc.sta.password)));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "STA connecting to '%s'", ssid);
    EventBits_t b = xEventGroupWaitBits(s_eg, BIT_CONNECTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(wait_ms));
    return (b & BIT_CONNECTED) != 0;
}

// ── captive DNS: answer every query with the AP IP so phones pop the portal ──
static void dns_task(void *arg)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY), .sin_port = htons(53) };
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) { close(s); vTaskDelete(NULL); }
    // 1s recv timeout so the loop can observe s_dns_run and exit when the softAP is
    // torn down (e.g. the user switches to app/BLE provisioning).
    struct timeval rcv_to = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rcv_to, sizeof(rcv_to));
    uint8_t buf[512];
    while (s_dns_run) {
        struct sockaddr_in ra; socklen_t rl = sizeof(ra);
        int n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&ra, &rl);
        if (n < 0) continue;   // timeout → re-check s_dns_run
        if (n < 12) continue;
        // Find QTYPE: skip the header (12) + QNAME labels (len,bytes,... ,0x00).
        int i = 12;
        while (i < n && buf[i]) { if (buf[i] & 0xC0) { i = n; break; } i += buf[i] + 1; }
        if (i + 4 >= n) continue;                   // malformed question
        int qtype = (buf[i + 1] << 8) | buf[i + 2]; // 1 = A, 28 = AAAA
        buf[2] |= 0x80; buf[3] = 0x80;              // QR=1 (response), RA=1
        if (qtype != 1) {                           // AAAA/other: valid EMPTY answer
            buf[6] = 0; buf[7] = 0;                 // ANCOUNT = 0 (so the client falls back to A)
            sendto(s, buf, n, 0, (struct sockaddr *)&ra, rl);
            continue;
        }
        buf[6] = 0; buf[7] = 1;                     // ANCOUNT = 1
        if (n + 16 > (int)sizeof(buf)) continue;
        uint8_t *p = buf + n;
        *p++ = 0xC0; *p++ = 0x0C;                   // name -> ptr to question
        *p++ = 0x00; *p++ = 0x01;                   // type A
        *p++ = 0x00; *p++ = 0x01;                   // class IN
        *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x3C;  // TTL 60
        *p++ = 0x00; *p++ = 0x04;                   // rdlength 4
        *p++ = 192; *p++ = 168; *p++ = 4; *p++ = 1; // 192.168.4.1
        sendto(s, buf, p - buf, 0, (struct sockaddr *)&ra, rl);
    }
    close(s);
    vTaskDelete(NULL);
}

// ── scan nearby networks for the portal SSID picker ──────────────────────────
#define SCAN_MAX 16
static char s_scan[SCAN_MAX][33];
static int  s_scan_n;

static void scan_networks(void)
{
    s_scan_n = 0;
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return;   // blocking, all channels
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (!n) return;
    wifi_ap_record_t *r = calloc(n, sizeof(*r));
    if (!r) { esp_wifi_clear_ap_list(); return; }
    esp_wifi_scan_get_ap_records(&n, r);                    // strongest RSSI first
    for (int i = 0; i < n && s_scan_n < SCAN_MAX; i++) {
        const char *ss = (const char *)r[i].ssid;
        if (!ss[0]) continue;                              // skip hidden
        int dup = 0;
        for (int j = 0; j < s_scan_n; j++) if (!strcmp(s_scan[j], ss)) { dup = 1; break; }
        if (!dup) { strncpy(s_scan[s_scan_n], ss, 32); s_scan[s_scan_n++][32] = 0; }
    }
    free(r);
    ESP_LOGI(TAG, "portal scan: %d networks", s_scan_n);
}

// ── portal HTTP ─────────────────────────────────────────────────────────────
// On brand with the on-device Claim/Wi-Fi screens: the NuVoxel blue gradient, the
// white wordmark, a translucent card holding the form. The wordmark is the same
// art as public/nuvoxel-wordmark.svg, inlined (fills forced white) — no external
// asset, since the softAP serves this with no internet and no bundled web files.
static const char PORTAL_HEAD[] =
"<!DOCTYPE html><html><head><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
"<title>NuVoxel \xE2\x80\x94 Wi-Fi Setup</title><style>"
"*{box-sizing:border-box}"
"body{font-family:system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;margin:0;min-height:100vh;color:#eef4fb;"
"background:linear-gradient(160deg,#312B63,#235E97);background-attachment:fixed;padding:34px 20px}"
".wrap{max-width:400px;margin:auto}"
"h2{font-weight:600;font-size:1.35rem;text-align:center;margin:0 0 4px}"
".sub{color:#bcd3ef;text-align:center;font-size:.9rem;margin:0 0 20px}"
".card{background:rgba(255,255,255,.07);border:1px solid rgba(255,255,255,.14);border-radius:16px;padding:20px}"
"label{display:block;font-size:.82rem;color:#cfe0f5;margin:12px 0 6px}"
"input,select{width:100%;padding:12px;border-radius:10px;border:1px solid rgba(255,255,255,.22);"
"background:rgba(255,255,255,.10);color:#fff;font-size:1rem}"
"input::placeholder{color:#a9c2de}"
"button{width:100%;margin-top:20px;padding:14px;border:0;border-radius:10px;background:#32B4E5;color:#06222b;"
"font-weight:700;font-size:1rem}"
"</style></head><body><div class=wrap>"
"<svg viewBox='0 0 77.327 11.758' style='width:62%;max-width:280px;display:block;margin:4px auto 20px'><g transform='matrix(.26458 0 0 .26458 134.58 -118.01)'><path fill='#fff' d='m-480.14 446.45h5.4316v42.959h-5.4316l-23.084-33.145v33.145h-5.4316v-42.959h5.4316l23.084 33.145zm37.218 43.699c-5.1847 0-9.2089-1.6583-12.073-4.9748-2.8557-3.3083-4.2835-7.8223-4.2835-13.542v-25.183h5.4316v24.998c0 4.1971 0.93406 7.5177 2.8022 9.9619 1.8764 2.4525 4.5839 3.6787 8.1226 3.6787 3.5387 0 6.2422-1.2262 8.1103-3.6787 1.8764-2.4442 2.8145-5.7648 2.8145-9.9619v-24.998h5.4316v25.183c0 5.7196-1.432 10.234-4.2959 13.542-2.8557 3.3165-6.8758 4.9748-12.061 4.9748z'/><path fill='#fff' d='m-386.45 446.77h5.4439l-15.739 42.959h-6.7894l-15.739-42.959h5.7402l13.579 36.972zm31.355 43.699c-4.156 0-7.7029-0.9258-10.641-2.7775-2.9462-1.8517-5.1888-4.444-6.7277-7.777-1.5472-3.333-2.3208-7.2215-2.3208-11.666s0.77358-8.3325 2.3208-11.666c1.5389-3.333 3.7815-5.9253 6.7277-7.777 2.938-1.8517 6.4849-2.7775 10.641-2.7775s7.707 0.9258 10.653 2.7775c2.938 1.8517 5.1806 4.444 6.7277 7.777 1.5389 3.333 2.3084 7.2215 2.3084 11.666s-0.76947 8.3325-2.3084 11.666c-1.5472 3.333-3.7897 5.9253-6.7277 7.777-2.9462 1.8517-6.4973 2.7775-10.653 2.7775zm0-4.9378c4.6086 0 8.065-1.5554 10.369-4.6662 2.3043-3.1025 3.4564-7.3079 3.4564-12.616s-1.1522-9.5175-3.4564-12.628c-2.3043-3.1026-5.7607-4.6539-10.369-4.6539s-8.065 1.5513-10.369 4.6539c-2.3043 3.1108-3.4564 7.3202-3.4564 12.628s1.1521 9.5135 3.4564 12.616c2.3043 3.1108 5.7607 4.6662 10.369 4.6662zm63.389 4.1971h-6.4808l-12.098-17.652-11.912 17.652h-6.1722l14.998-22.158-14.258-20.8h6.4808l11.357 16.542 11.172-16.542h6.1722l-14.258 21.047zm36.601-37.774h-20.183v13.579h18.825v4.9378h-18.825v14.073h20.183v5.1846h-25.615v-42.959h25.615zm13.887 37.774v-42.959h5.4316v37.774h19.381v5.1846z'/></g></svg>"
"<h2>Wi-Fi Setup</h2>"
/* device name (which panel you're setting up) goes here at runtime -- see portal_get() */
"<p class=sub>";   /* closed in portal_get() */
static const char PORTAL_TAIL[] =
"<label>Password</label><input name=pass type=password>"
"<button>Connect</button></form></div></div></body></html>";

// Minimal HTML-escape for SSIDs placed into <option> text/value.
static void html_esc(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; *in && o + 7 < cap; in++) {
        const char *r = (*in=='&')?"&amp;":(*in=='<')?"&lt;":(*in=='>')?"&gt;":(*in=='"')?"&quot;":NULL;
        if (r) { while (*r && o+1<cap) out[o++]=*r++; } else out[o++]=*in;
    }
    out[o] = 0;
}

static esp_err_t portal_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, PORTAL_HEAD);
    /* Name the panel you are standing in front of: with several keypads on one
     * site the portal is otherwise identical on every one of them. */
    httpd_resp_sendstr_chunk(req, device_model_name());
    httpd_resp_sendstr_chunk(req,
        "</p><div class=card><form method=POST action=/connect><label>Network</label>"
        "<select name=ssid id=sel onchange=\"o.style.display=sel.value?'none':'block'\">");
    char esc[132], opt[300];
    for (int i = 0; i < s_scan_n; i++) {
        html_esc(s_scan[i], esc, sizeof(esc));
        snprintf(opt, sizeof(opt), "<option value=\"%s\">%s</option>", esc, esc);
        httpd_resp_sendstr_chunk(req, opt);
    }
    // "Other" (value empty) reveals a manual field — for hidden/out-of-range nets.
    snprintf(opt, sizeof(opt),
        "<option value=\"\">Other / hidden\xE2\x80\xA6</option></select>"
        "<input id=o name=ssidm placeholder=\"Network name\" style=\"display:%s\">",
        s_scan_n ? "none" : "block");
    httpd_resp_sendstr_chunk(req, opt);
    httpd_resp_sendstr_chunk(req, PORTAL_TAIL);
    httpd_resp_sendstr_chunk(req, NULL);          // end chunked response
    return ESP_OK;
}

static int hexv(char c) { return (c>='0'&&c<='9')?c-'0':(c>='a'&&c<='f')?c-'a'+10:(c>='A'&&c<='F')?c-'A'+10:-1; }
static void urldecode(char *s)                    // in place: %XX and '+' -> space
{
    char *d = s;
    while (*s) {
        if (*s=='+') { *d++=' '; s++; }
        else if (*s=='%' && hexv(s[1])>=0 && hexv(s[2])>=0) { *d++=(char)((hexv(s[1])<<4)|hexv(s[2])); s+=3; }
        else *d++=*s++;
    }
    *d = 0;
}

static esp_err_t portal_connect(httpd_req_t *req)
{
    char body[256];
    int n = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int got = httpd_req_recv(req, body, n);
    if (got <= 0) { httpd_resp_send_500(req); return ESP_OK; }
    body[got] = 0;
    char ssid[65] = {0}, ssidm[65] = {0}, pass[65] = {0};
    httpd_query_key_value(body, "ssid", ssid, sizeof(ssid));    // picked from the list
    httpd_query_key_value(body, "ssidm", ssidm, sizeof(ssidm)); // manual ("Other")
    httpd_query_key_value(body, "pass", pass, sizeof(pass));
    urldecode(ssid); urldecode(ssidm); urldecode(pass);
    const char *use = ssidm[0] ? ssidm : ssid;                 // manual wins when present
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<style>body{font-family:system-ui,-apple-system,sans-serif;margin:0;min-height:100vh;color:#eef4fb;"
        "display:flex;align-items:center;justify-content:center;text-align:center;padding:24px;"
        "background:linear-gradient(160deg,#312B63,#235E97)}"
        "p{max-width:320px;font-size:1.05rem;line-height:1.5}</style></head>"
        "<body><p>Connecting\xE2\x80\xA6<br>the keypad will switch to its home screen once it's on your network.</p></body></html>");
    ESP_LOGI(TAG, "portal creds for '%s'", use);
    // Try them (creds persist to NVS via esp_wifi storage=flash on success).
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    wifi_config_t wc = {0};
    memcpy(wc.sta.ssid, use, strnlen(use, sizeof(wc.sta.ssid)));
    memcpy(wc.sta.password, pass, strnlen(pass, sizeof(wc.sta.password)));
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    s_hold_sta = false;   // creds submitted — allow on_evt to (re)connect/retry the STA
    esp_wifi_connect();
    return ESP_OK;
}

// Captive catch-all: every non-root probe URL (Apple's /hotspot-detect.html,
// Android's /generate_204, MS /connecttest.txt, …) gets a 302 to our portal.
// A 302 is what actually makes iOS's CNA auto-pop the sign-in sheet — a plain
// 200-with-HTML is interpreted as "online" and the sheet never appears (the
// user then has to open a browser by hand). Android/Windows treat the 302 (not
// their expected 204/200) as "captive" and raise their own sign-in prompt.
static esp_err_t portal_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "redirect", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void start_portal(void)
{
    if (s_portal) return;   // already up (idempotent)
    s_hold_sta = true;      // portal up with no creds — keep the STA quiet so the AP beacons
    ESP_LOGI(TAG, "starting setup portal AP '%s'", s_ap);
    // Create the AP netif once and reuse it across stop/restart (re-creating the
    // default AP netif leaks/errors). start/stop just toggle the AP radio + services.
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_t *ap_netif = s_ap_netif;
    wifi_config_t ap = {0};
    /* ssid is a 32-byte field carried with an explicit ssid_len, NOT a C string —
     * a full 32-char SSID legitimately has no terminator, so copy by length. */
    size_t ssid_len = strnlen(s_ap, sizeof(ap.ap.ssid));
    memcpy(ap.ap.ssid, s_ap, ssid_len);
    ap.ap.ssid_len = ssid_len;
    ap.ap.max_connection = 4;
    // WPA2-PSK with the per-device password (carried in the QR). Secured setup APs
    // get much more reliable iOS captive-portal auto-popup than open ones.
    strncpy((char *)ap.ap.password, s_pass, sizeof(ap.ap.password) - 1);
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.channel = 1;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Make the softAP's DHCP advertise ITS OWN IP (192.168.4.1) as the DNS
    // server. Without this a joining phone uses no/other DNS, so its captive-
    // portal probe (captive.apple.com / connectivitycheck.gstatic.com / ...)
    // never resolves to us, dns_task never sees it, and the portal never pops.
    // With it: probe domain -> 192.168.4.1 -> our wildcard HTTP serves the
    // portal (non-"Success") -> iOS opens the sign-in sheet, Android prompts.
    esp_netif_dns_info_t dns = { .ip = { .type = ESP_IPADDR_TYPE_V4 } };
    dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(192, 168, 4, 1);
    uint8_t dns_offer = 0x02;   // OFFER_DNS bit — tell dhcps to send option 6
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                           ESP_NETIF_DOMAIN_NAME_SERVER, &dns_offer, sizeof(dns_offer));
    esp_netif_dhcps_start(ap_netif);

    // Scan nearby networks now (AP up, no client yet → the scan's channel hop
    // disrupts nobody) so the portal can offer a picker instead of typed SSIDs.
    scan_networks();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_portal, &cfg) == ESP_OK) {
        httpd_uri_t conn = { .uri = "/connect", .method = HTTP_POST, .handler = portal_connect };
        httpd_uri_t root = { .uri = "/",     .method = HTTP_GET, .handler = portal_get };       // the actual portal page
        httpd_uri_t any  = { .uri = "/*",    .method = HTTP_GET, .handler = portal_redirect };  // probes -> 302 -> /
        httpd_register_uri_handler(s_portal, &conn);
        httpd_register_uri_handler(s_portal, &root);   // register the exact "/" BEFORE the wildcard
        httpd_register_uri_handler(s_portal, &any);
    }
    s_dns_run = true;
    xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL);
}

// Tear the softAP down: stop the HTTP portal + captive DNS task and drop the AP
// radio (STA stays up). Frees the portal/DNS/AP RAM — the headroom native BLE needs
// on the RAM-tight S3. The AP netif object is kept for a later start_portal().
static void stop_portal(void)
{
    if (s_portal) { httpd_stop(s_portal); s_portal = NULL; }
    s_dns_run = false;                       // dns_task times out within ~1s and exits
    esp_wifi_set_mode(WIFI_MODE_STA);        // drop the AP; keep STA for the eventual join
    ESP_LOGI(TAG, "setup portal AP stopped");
}


void wifi_start(void)
{
    s_eg = xEventGroupCreate();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_evt, NULL, NULL));

    uint8_t mac[6];
#if CONFIG_IDF_TARGET_ESP32P4
    // The WiFi MAC lives on the C6 co-processor (esp_wifi_remote): esp_read_mac()
    // reads the local P4 efuse, which has no WiFi MAC (returns zeros). Ask the
    // remote radio — valid here since we're after esp_wifi_init().
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK)
        esp_read_mac(mac, ESP_MAC_BASE);
#else
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
#endif
#if MMK_WIFI_MAC_OVERRIDE
    // Workaround for a controller-side block tied to the factory MAC: derive a NEW,
    // still-unique STA MAC by setting the locally-administered bit (standards-correct
    // self-assigned MAC). Unique per unit since it's derived from the unique factory
    // MAC. Must be set after esp_wifi_init() and before start (we're between them).
    uint8_t nmac[6];
    memcpy(nmac, mac, sizeof(nmac));
    nmac[0] = (uint8_t)((nmac[0] | 0x02) & 0xFE);   // locally administered + unicast
    if (esp_wifi_set_mac(WIFI_IF_STA, nmac) == ESP_OK) memcpy(mac, nmac, sizeof(mac));
    ESP_LOGW(TAG, "STA MAC override -> %02x:%02x:%02x:%02x:%02x:%02x (was factory)",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
#endif
    /* SSID carries the board model, matching the SDDP Host/Model and the BLE name
     * (all via device_variant_tag) so a unit is recognisable everywhere by the
     * same string: "T3-10-82A3A6", "S3-1A2B3C". */
    {
        char tag[24];
        device_variant_tag(tag, sizeof(tag));
        snprintf(s_ap_pop, sizeof(s_ap_pop), "%02X%02X%02X", mac[3], mac[4], mac[5]);
        snprintf(s_ap, sizeof(s_ap), "%s-%s", tag, s_ap_pop);
    }
    // WPA2 PSK = the full MAC as 12 lowercase hex (unique per unit, ≥8 chars). The
    // phone auto-fills it from the QR, so the user never sees or types it. A secured
    // setup AP makes iOS's captive-portal auto-popup far more reliable than an open one.
    snprintf(s_pass, sizeof(s_pass), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

#if defined(MMK_C6_OTA) || defined(MMK_BLE_SPIKE)
    // Bench spike builds (C6 slave OTA / BLE advertise): only bring the esp_hosted
    // SDIO transport to the C6 up (start STA — no association needed) so app_main's
    // c6_ota_run() / ble_spike_start() can reach the slave over RPC/HCI. Skip the
    // creds/portal path (no WiFi association required for either).
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    vTaskDelay(pdMS_TO_TICKS(4000));   // let the SDIO link enumerate + slave identify
    return;
#endif
    // 1) Dev override: bench Kconfig creds.
    if (strlen(CONFIG_MMKEYPAD_BENCH_WIFI_SSID) > 0) {
        try_sta(CONFIG_MMKEYPAD_BENCH_WIFI_SSID, CONFIG_MMKEYPAD_BENCH_WIFI_PASS, 20000);
        return;
    }
    // 2) Saved creds from a previous provisioning (persisted in NVS by esp_wifi).
    wifi_config_t saved = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &saved) == ESP_OK && saved.sta.ssid[0]) {
        if (try_sta((char *)saved.sta.ssid, (char *)saved.sta.password, 15000)) return;
    }
    // 3) First boot / failed: run the onboarding paths until connected. On BLE
    //    boards we offer BOTH the ESP BLE-provisioning path AND the softAP captive
    //    portal at once — the user picks whichever they prefer, and BIT_CONNECTED
    //    (set on IP_EVENT_STA_GOT_IP) fires from whichever path wins. On non-BLE
    //    boards it's portal-only, exactly as before.
#ifdef MMK_HAS_BLE_PROV
    // Offer BOTH paths concurrently (C6 boards have the RAM): BLE app provisioning
    // AND the softAP captive portal. The pop = the 6-hex MAC suffix of the SSID
    // (s_ap_pop -- NOT an offset into s_ap; the model prefix is variable length),
    // carried in the on-screen QR. s_prov_active gates on_evt's STA auto-connect off
    // while the manager owns the STA.
    s_prov_active = true;
    prov_start(s_ap, s_ap_pop);
    if (lvgl_port_lock(0)) { ui_show_setup(s_ap, s_pass, s_ap_pop); lvgl_port_unlock(); }
#else
    if (lvgl_port_lock(0)) { ui_show_setup(s_ap, s_pass, NULL); lvgl_port_unlock(); }
#endif
    start_portal();
    xEventGroupWaitBits(s_eg, BIT_CONNECTED, pdFALSE, pdTRUE, portMAX_DELAY);
#ifdef MMK_HAS_BLE_PROV
    s_prov_active = false;         // connected — restore normal reconnect handling
    prov_stop();                   // stop + deinit the prov manager (idempotent)
#endif
    stop_portal();                 // tear down softAP + captive DNS
    if (lvgl_port_lock(0)) { ui_hide_setup(); lvgl_port_unlock(); }
}

bool wifi_is_up(void) { return s_up; }
