// MMKeypad — Phase 3 intercom SIP user agent (esp_rtc).
// The intercom control protocol is multiplexed onto the SAME :6700 socket as the
// now-playing/keypad protocol (net.c) — there's no second TCP channel, so the
// keypad driver's single network binding serves intercom too (one Composer bind).
// net.c routes `sip`/`call` messages here via sip_handle(); we send `sipstate` /
// `callstate` back over the same link via net_send_line().
//
// The driver provisions SIP creds (`sip` msg) → we register with the SIP server
// (Director) via esp_rtc and bridge calls: inbound INVITEs become calls whose
// audio runs through the ES8311 (audio.c). esp_rtc gives raw-PCM audio callbacks,
// so no ESP-ADF is needed — send_audio pulls from the mic, receive_audio plays to
// the speaker, both via audio.c.
//
// SCOPE. esp_rtc owns the SIP dialogs, the SDP offer/answer and the RTP session;
// this file is glue — creds in, audio and UI events out. Everything about staying
// reachable (keepalive, re-REGISTER, media timeout) is CONFIGURATION on esp_rtc,
// not tasks of ours: see the block above sip_apply(). The T3's libre-based UA
// (firmware-linux-t3/platform/sip_linux.c) has the same public contract and the
// same wire behaviour, but there we drive the stack directly and the compliance
// details (Allow, OPTIONS, RTCP, 603 Decline) are ours to get right.

#include "sip.h"
#include "audio.h"
#include "net.h"
#include "board.h"
#ifdef PIN_RGB_LED
#include "halo.h"
#define HALO_PULSE(on) halo_pulse(on)     // blue ring breathe on the onboard LED
#else
#define HALO_PULSE(on) ((void)0)
#endif

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "cJSON.h"

#include "esp_rtc.h"
#include "media_lib_adapter.h"

static const char *TAG = "sip";

#define CALL_RATE_HZ   8000          // G.711 intercom

// ── liveness (esp_rtc config, not a task of ours) ───────────────────────────
// The registrar has to keep believing in us, and we have to notice when it stops:
// inbound INVITEs arrive over the connection WE opened (transport is tcp, pushed
// by the driver — see SIP_TRANSPORT in driver-intercom), so a flow that dies
// unnoticed makes the panel silently unreachable while it still looks registered.
// esp_rtc does both natively — OPTIONS pings (RFC 3261 §11, a real request that
// gets a real response, unlike a bare CRLF whose loss is invisible) plus its own
// re-REGISTER on the interval below — so none of this is our code to write.
//
// It is also why there is no longer an RTP watchdog task here. That task ended any
// call that went 5 s without an incoming RTP packet, to catch a BYE that never
// arrived. But silence is not a teardown signal — a peer doing VAD/silence
// suppression legitimately stops sending, and the rule hung up on live, quiet
// calls. The failure it was reaching for is a SIGNALLING failure, which the
// keepalive below actually detects; esp_rtc's own media timeout still backstops a
// call whose RTP genuinely stops. (RFC 4028 session timers would be the
// standards-track way to expire a stuck dialog; esp_rtc exposes no knob for them.)
#define KEEPALIVE_SECS 30
#define REG_INTERVAL_S 600
static bool              s_inited;          // sip_init() ran (audio was ready)
static bool              s_in_call;         // an audio session is active
static bool              s_incoming;        // an inbound call is ringing (dedupes INCOMING)
static esp_rtc_handle_t  s_rtc;
static bool              s_auto_answer = true; // announcements auto-answer by default
static bool              s_monitor;            // monitor mode: silent auto-answer (no beep)
static bool              s_play_door_chime = true; // door calls ring the doorbell chime (proxy setting)
static bool              s_muted;              // MUTE_CALL: stop sending mic audio
static char              s_uri[160];
static char              s_local_ip[16];
static sip_call_cb_t     s_call_cb;         // on-screen call UI bridge (optional)
static bool              s_ringing_call;    // ringing an unanswered inbound call

void sip_set_call_cb(sip_call_cb_t cb) { s_call_cb = cb; }
void sip_set_mute(bool on) { s_muted = on; }
bool sip_is_muted(void)     { return s_muted; }

void sip_answer(void) { if (s_rtc) esp_rtc_answer(s_rtc); }
void sip_hangup(void) { if (s_rtc) esp_rtc_bye(s_rtc); }
// Place an outbound intercom call to a SIP username (from the on-screen Intercom picker).
// Friendly name for the current peer. esp_rtc_get_peer() reports the transport's
// idea of the peer, which rendered as "intercom" for every outbound call.
static char s_peer_name[80];

void sip_place_call(const char *user, const char *dname)
{
    if (!(s_rtc && !s_in_call && user && user[0])) return;
    snprintf(s_peer_name, sizeof(s_peer_name), "%s", (dname && dname[0]) ? dname : user);
    esp_rtc_call(s_rtc, user);
}

// Notify the local UI of a call-state change ("incoming"/"outgoing"/"active"/"ended").
static void ui_call_event(const char *event)
{
    if (!s_call_cb) return;
    const char *peer = s_peer_name[0] ? s_peer_name
                                      : (s_rtc ? esp_rtc_get_peer(s_rtc) : NULL);
    s_call_cb(event, peer);
    if (!strcmp(event, "ended")) s_peer_name[0] = '\0';
}

// ── send to the driver (over net.c's :6700 socket) ──────────────────────────
static void send_obj(cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    if (s) { net_send_line(s); cJSON_free(s); }
    cJSON_Delete(o);
}

static void send_sipstate(bool registered)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "sipstate");
    cJSON_AddBoolToObject(o, "registered", registered);
    send_obj(o);
}

static void send_callstate(const char *event)
{
    const char *peer = s_rtc ? esp_rtc_get_peer(s_rtc) : NULL;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "callstate");
    cJSON_AddStringToObject(o, "event", event);
    if (peer && peer[0]) cJSON_AddStringToObject(o, "remoteAor", peer);
    send_obj(o);
}

// ── esp_rtc audio callbacks (raw PCM ↔ ES8311 via audio.c) ───────────────────
static int cb_send_audio(unsigned char *data, int len, void *ctx)
{
    if (s_muted) return 0;                   // MUTE_CALL: send no mic audio upstream
    int n = audio_call_read(data, len);     // mic PCM, resampled 16k stereo -> 8k mono
    return n > 0 ? n : 0;
}

static int cb_receive_audio(unsigned char *data, int len, void *ctx)
{
    // Belt and braces: with cb_receive_dtmf registered, esp_rtc delivers RFC 4733
    // events there instead of inlining them here as a 6-byte "DTMF-"+id payload.
    // The check stays because feeding an event packet to the µ-law decoder is a
    // burst of noise in the speaker, and that is a bad way to find out the
    // component changed its mind.
    if (len == 6 && strncasecmp((char *)data, "DTMF-", 5) == 0) return 0;
    int n = audio_call_write(data, len);    // remote 8k mono PCM -> 16k stereo speaker
    return n > 0 ? n : len;
}

// RFC 4733 telephone-event. We have no dialplan to feed, so this only exists to
// keep DTMF out of the audio path (and to show up in a log when a door station
// sends one).
static int cb_receive_dtmf(unsigned char *data, int len, void *ctx)
{
    ESP_LOGI(TAG, "dtmf: %.*s", len, (const char *)data);
    return 0;
}

// ── door-station classification ─────────────────────────────────────────────
// The driver resolves which intercom endpoints are door stations (Control4's own
// `isDoorStation` capability, via the proxy) and pushes their SIP usernames here as a
// "|user1|user2|…|" delimited set. A call from one of these is a doorbell → doorbell
// chime + manual answer, never auto-answered/monitored. No peer-name heuristics.
static char *s_doorstations;   // "|user|user|…" or NULL

void sip_set_doorstations(const char *delimited)
{
    free(s_doorstations);
    s_doorstations = (delimited && delimited[0]) ? strdup(delimited) : NULL;
    ESP_LOGI(TAG, "door stations: %s", s_doorstations ? s_doorstations : "(none)");
}

static bool peer_is_doorstation(const char *peer)
{
    if (!peer || !peer[0] || !s_doorstations) return false;
    char user[80];                                  // SIP user part (before '@' if any)
    size_t i = 0;
    for (; peer[i] && peer[i] != '@' && i + 1 < sizeof(user); i++) user[i] = peer[i];
    user[i] = '\0';
    char needle[84];
    snprintf(needle, sizeof(needle), "|%s|", user);
    return strstr(s_doorstations, needle) != NULL;
}

// ── esp_rtc session events → state + (auto)answer ───────────────────────────
static int cb_event(esp_rtc_event_t event, void *ctx)
{
    switch (event) {
    case ESP_RTC_EVENT_REGISTERED:
        ESP_LOGI(TAG, "registered");
        send_sipstate(true);
        break;
    case ESP_RTC_EVENT_UNREGISTERED:
        ESP_LOGW(TAG, "unregistered");
        send_sipstate(false);
        break;
    case ESP_RTC_EVENT_INCOMING: {
        // ONCE per call. esp_rtc re-reports INCOMING for as long as the caller keeps
        // ringing (its own header says so: "reported periodically during ringing;
        // handle auto-answer only once"). Unguarded, that restarted the ring tone
        // from the top every few seconds instead of letting it play its cadence,
        // re-fired the blocking heads-up beep, and re-sent esp_rtc_answer() into an
        // already-answered dialog. s_incoming clears in HANGUP/ERROR/SESSION_BEGIN,
        // i.e. every way a call can leave the ringing state.
        if (s_incoming) break;
        s_incoming = true;
        // Doorbell = the caller is a door station, per the set the driver pushed (derived
        // from Control4's isDoorStation capability). Vendor-neutral, no peer-name matching.
        const char *peer = esp_rtc_get_peer(s_rtc);
        bool is_door = peer_is_doorstation(peer);
        ESP_LOGI(TAG, "incoming call from %s (autoAnswer=%d monitor=%d door=%d)",
                 peer ? peer : "?", s_auto_answer, s_monitor, is_door);
        send_callstate("incoming");
        if (is_door) {
            // Door station: manual answer, never auto-answered/monitored. Ring the doorbell
            // chime if the proxy's Play Door Chime is on, else the normal ring.
            ui_call_event("incoming");
            audio_ring_start_ex(s_play_door_chime);
            HALO_PULSE(true);
            s_ringing_call = true;
        } else if (s_monitor) {
            esp_rtc_answer(s_rtc);             // monitor: silent pickup, no beep/ring
        } else if (s_auto_answer) {
            audio_play_beep();                 // heads-up beep — BLOCKING so it sounds
            esp_rtc_answer(s_rtc);             // before the call audio reconfigures the codec
        } else {
            // Manual: ring + on-screen Answer/Decline (the driver is passive, so the
            // Director just waits on the SIP dialog; tapping Answer sends 200 OK).
            ui_call_event("incoming");
            audio_ring_start();
            HALO_PULSE(true);                  // blue breathe while ringing
            s_ringing_call = true;
        }
        break;
    }
    case ESP_RTC_EVENT_CALLING:
        // Local ringback while the far end rings. Without it, placing a call is
        // silent and indistinguishable from a call that never went out.
        // s_ringing_call MUST be set too: every audio_ring_stop() below is gated
        // on it, so without this the ringback would play on into the answered
        // call and never stop.
        audio_ringback_start();
        s_ringing_call = true;
        send_callstate("outgoing");
        ui_call_event("outgoing");
        break;
    case ESP_RTC_EVENT_CALL_ANSWERED:
        // Fires only for the CALLER when the remote picks up. The "connected"
        // handling (UI/beep/ring-stop) lives in AUDIO_SESSION_BEGIN, which fires
        // for BOTH directions — incl. an incoming call we (auto-)answer.
        break;
    case ESP_RTC_EVENT_AUDIO_SESSION_BEGIN:
        s_incoming = false;
        if (s_ringing_call) { audio_ring_stop(); s_ringing_call = false; }
        HALO_PULSE(false);                          // stop blue ring breathe
        audio_call_begin(CALL_RATE_HZ);             // 16 kHz stereo codec + amp on
        s_in_call = true;
        send_callstate("accepted");
        ui_call_event("active");                    // -> "Connected" + End on screen
        break;
    case ESP_RTC_EVENT_AUDIO_SESSION_END:
        s_in_call = false;
        audio_call_end();                   // restore UI format + amp off
        break;
    case ESP_RTC_EVENT_HANGUP:
        ESP_LOGI(TAG, "hangup");
        s_in_call = s_incoming = false;
        if (s_ringing_call) {                       // declined / missed while ringing
            audio_ring_stop(); audio_amp(false); s_ringing_call = false;
        }
        HALO_PULSE(false);                          // stop blue ring breathe
        send_callstate("ended");
        ui_call_event("ended");
        break;
    case ESP_RTC_EVENT_ERROR:
        ESP_LOGE(TAG, "rtc error");
        s_in_call = s_incoming = false;
        if (s_ringing_call) {
            audio_ring_stop(); audio_amp(false); s_ringing_call = false;
        }
        ui_call_event("ended");                     // clear the call UI on error
        break;
    default:
        break;
    }
    return 0;
}

// ── (re)register with provisioned creds ─────────────────────────────────────
static void local_ip(void)
{
    esp_netif_t *nif = mmk_default_netif();
    esp_netif_ip_info_t ip = {0};
    if (nif) esp_netif_get_ip_info(nif, &ip);
    snprintf(s_local_ip, sizeof(s_local_ip), IPSTR, IP2STR(&ip.ip));
}

static void sip_apply(const char *server, int port, const char *transport,
                      const char *user, const char *pass, bool auto_answer)
{
    // Reject EMPTY as well as NULL. This guard used to be a bare null check, so a
    // `sip` message carrying user="" (what the Control4 driver sends before anyone
    // configures an intercom account) built the malformed URI "tcp://:@host:5060"
    // and esp_rtc crashed on it ~6s later, during registration — rebooting the panel
    // and, because the driver re-pushes on every reconnect, boot-looping it.
    // A bad message from the network must never be able to take the device down.
    if (!server || !server[0] || !user || !user[0]) {
        ESP_LOGW(TAG, "sip: ignoring config with empty server/user (server='%s' user='%s')",
                 server ? server : "(null)", user ? user : "(null)");
        return;
    }
    s_auto_answer = auto_answer;

    local_ip();
    if (!strcmp(s_local_ip, "0.0.0.0")) {
        // No IP yet. Registering from 0.0.0.0 puts that address in the Contact and
        // in the SDP c= line, so the registrar accepts us and then has nowhere to
        // send an INVITE or a single RTP packet. The driver re-pushes on every
        // reconnect, so dropping this one costs nothing.
        ESP_LOGW(TAG, "no local IP yet — deferring SIP registration");
        return;
    }

    char uri[sizeof(s_uri)];
    snprintf(uri, sizeof(uri), "%s://%s:%s@%s:%d",
             (transport && transport[0]) ? transport : "udp",
             user, pass ? pass : "", server, port > 0 ? port : 5060);
    // The driver re-pushes `sip` on every link-up, and tearing the stack down to
    // rebuild it identically drops any call in progress and makes us re-REGISTER
    // for no reason. Only rebuild when something actually changed. (auto-answer
    // rides along on this message but is pure local behaviour — set above, before
    // the early-out, so a toggle still lands.)
    if (s_rtc && !strcmp(uri, s_uri)) {
        ESP_LOGI(TAG, "sip: config unchanged — keeping the current registration");
        return;
    }
    if (s_rtc) { esp_rtc_service_deinit(s_rtc); s_rtc = NULL; }
    snprintf(s_uri, sizeof(s_uri), "%s", uri);

    static esp_rtc_data_cb_t data_cb = {
        .send_audio = cb_send_audio,
        .receive_audio = cb_receive_audio,
        .receive_dtmf = cb_receive_dtmf,
    };
    // Explicitly declare "no video" so esp_rtc rejects the caller's video m-line
    // instead of spinning up RTP video recv/send tasks (audio-only intercom).
    static esp_rtc_video_info_t no_video = { .vcodec = RTC_VCODEC_NULL };
    esp_rtc_config_t cfg = {
        .uri = s_uri,
        .local_addr = s_local_ip,
        .acodec_type = RTC_ACODEC_G711U,        // Control4 intercom = G.711 µ-law
        .vcodec_info = &no_video,
        .data_cb = &data_cb,
        .event_handler = cb_event,
        // User-Agent as RFC 3261 §20.41 means it: one product token with our
        // version. The old "M Keypad SIP/2.0" parsed as three products, the last
        // being a product named SIP at version 2.0 — the protocol version wearing
        // our software version's clothes. Matches the T3 build; nothing keys on it.
        .user_agent = "MMKeypad/1.0",
        .send_options = true,                   // OPTIONS ping, not a bare CRLF
        .keepalive = KEEPALIVE_SECS,
        .register_interval = REG_INTERVAL_S,
        // Don't let a registration refresh (and its auth round-trip) land in the
        // middle of a call — RFC 3261 keeps the two independent, and on a busy
        // link the re-REGISTER is exactly what stalls the media task.
        .suspend_reg_on_call = true,
    };
    s_rtc = esp_rtc_service_init(&cfg);
    ESP_LOGI(TAG, "registering: %s://%s@%s:%d (auto_answer=%d)",
             (transport && transport[0]) ? transport : "udp", user, server,
             port > 0 ? port : 5060, auto_answer);
}

// ── driver messages ─────────────────────────────────────────────────────────
static const char *jstr(const cJSON *o, const char *k, const char *def)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : def;
}

// ── control messages (routed from net.c's :6700 handler) ────────────────────
void sip_handle(const cJSON *d)
{
    if (!s_inited || !d) return;          // audio wasn't ready → SIP never came up
    const char *t = jstr(d, "t", "");
    if (!strcmp(t, "sip")) {
        const cJSON *p = cJSON_GetObjectItem(d, "port");
        const cJSON *aa = cJSON_GetObjectItem(d, "autoAnswer");
        sip_apply(jstr(d, "server", NULL), cJSON_IsNumber(p) ? p->valueint : 5060,
                  jstr(d, "transport", "udp"), jstr(d, "user", NULL),
                  jstr(d, "pass", ""), cJSON_IsBool(aa) ? cJSON_IsTrue(aa) : true);
    } else if (!strcmp(t, "callcfg")) {
        // Live call-behavior config from the driver (Auto Answer / Monitor Mode),
        // separate from `sip` so toggling it doesn't re-register. No re-register.
        const cJSON *aa  = cJSON_GetObjectItem(d, "autoAnswer");
        const cJSON *mon = cJSON_GetObjectItem(d, "monitor");
        const cJSON *mut = cJSON_GetObjectItem(d, "mute");
        const cJSON *pdc = cJSON_GetObjectItem(d, "playDoorChime");
        if (cJSON_IsBool(aa))  s_auto_answer = cJSON_IsTrue(aa);
        if (cJSON_IsBool(mon)) s_monitor = cJSON_IsTrue(mon);
        if (cJSON_IsBool(mut)) s_muted = cJSON_IsTrue(mut);
        if (cJSON_IsBool(pdc)) s_play_door_chime = cJSON_IsTrue(pdc);
        ESP_LOGI(TAG, "callcfg: autoAnswer=%d monitor=%d mute=%d doorChime=%d",
                 s_auto_answer, s_monitor, s_muted, s_play_door_chime);
    } else if (!strcmp(t, "call") && s_rtc) {
        const char *a = jstr(d, "action", "");
        ESP_LOGI(TAG, "relay call action: %s", a);   // DIAG: proxy-driven accept/end?
        if (!strcmp(a, "accept")) {
            if (s_ringing_call) { audio_ring_stop(); s_ringing_call = false; }
            esp_rtc_answer(s_rtc);
        }
        else if (!strcmp(a, "start"))  esp_rtc_call(s_rtc, jstr(d, "remote", ""));
        else if (!strcmp(a, "end") || !strcmp(a, "reject")) esp_rtc_bye(s_rtc);
    }
    // unknown types ignored (forward-compat)
}

void sip_init(void)
{
    // Registers media_lib's malloc/thread adapters that esp_rtc allocates through
    // (large buffers land in PSRAM via SPIRAM_USE_MALLOC). Without this,
    // esp_rtc_service_init reports "Memory exhausted".
    media_lib_add_default_adapter();
    s_inited = true;
    ESP_LOGI(TAG, "intercom SIP ready (control over :6700)");
}
