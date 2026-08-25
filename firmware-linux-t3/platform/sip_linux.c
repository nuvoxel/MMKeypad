/* SIP user agent for the T3 (RK3188 / Linux), on libre.
 *
 * The native counterpart of firmware-idf/main/sip.c: same public contract
 * (sip.h) and the same wire behaviour toward Control4, but built on libre
 * (BSD-licensed, thirdparty/libre) instead of esp_rtc.
 *
 * WHY libre AND NOT LINPHONE, even though Control4's own intercom is linphone
 * (phoenix-navigator.apk ships liblinphone/mediastreamer2): liblinphone is GPLv3,
 * which is a problem for shipped firmware, and it is ~12 MB of C++ against our
 * 2.2 MB static C binary. Interop does not require it -- the ESP32 keypad already
 * talks to Control4 with esp_rtc, a third unrelated stack. linphone stays useful
 * as a behavioural reference when something does not interop.
 *
 * THREADING. libre is single-threaded: every libre call must happen on the thread
 * running re_main(). sip_init() starts that thread; everything arriving from
 * elsewhere (net.c's :6700 handler, the LVGL UI) is marshalled onto it through an
 * mqueue. Do not call libre directly from another thread -- it will appear to work
 * and then corrupt the event loop under load.
 *
 * TRANSPORT IS TCP, deliberately. Registration works over UDP, but inbound
 * INVITEs never arrive: the panel sits behind AP isolation/NAT where new inbound
 * UDP is dropped. Control4's own stations use persistent connections for exactly
 * this reason (UniFi=TCP, native door station=TLS), so inbound rides the socket we
 * opened. See PHASE3-INTERCOM-SIP.md.
 *
 * STAYING REGISTERED is libre's job, not ours. sipreg refreshes at 90% of the
 * expiry the REGISTRAR granted (not the one we asked for) and, on any failure,
 * retries with the RFC-style exponential backoff + jitter in sipreg's failwait().
 * The one thing it cannot see is a TCP flow that died without a FIN -- and because
 * inbound INVITEs ride that flow, a half-dead socket makes the panel silently
 * unreachable while the kernel still calls it ESTABLISHED. That is what the
 * RFC 5626 §4.4.1 CRLF keepalive below is for: no pong inside libre's 10 s window
 * and libre tears the connection out of its pool, which both frees us to open a
 * fresh one and calls ka_handler() so we re-REGISTER over it immediately.
 *
 * This replaced a hand-rolled reg_watchdog + sip_stack_rebuild pair that polled
 * s_registered every 60 s and, after two strikes, tore down and re-allocated the
 * whole stack. It was reinventing sipreg's retry (badly -- no backoff, so a
 * registrar that was down got hammered once a minute forever) around one real
 * gap, which is the flow liveness the keepalive now covers directly.
 */
#include "sip.h"

#if MMK_HAS_SIP

#include "audio.h"
#include "net.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <re.h>
#include <rem.h>
#include "cJSON.h"

/* G.711 µ-law: payload type 0, 8 kHz, 20 ms packets = 160 samples/bytes. */
#define PT_PCMU        0
#define RTP_RATE       8000
#define PTIME_MS       20
#define SAMPLES_PER_PKT (RTP_RATE * PTIME_MS / 1000)   /* 160 */

/* What we ASK for; the registrar's granted value (its Contact ;expires, or its
 * Expires header) is what libre actually refreshes against. 10 min is a
 * compromise, not a liveness mechanism: a dead flow is caught by the keepalive
 * in ~1 min, so the only failure a short expiry still covers is the registrar
 * dropping our binding while the TCP stays up (a FreeSWITCH registration flush).
 * That is rare enough not to justify re-registering every 5 min forever. */
#define REG_EXPIRES    600
/* CRLF keepalive on the registrar's flow (RFC 5626 §4.4.1 double-CRLF ping, the
 * peer answers with a single CRLF). libre floors this at 2× its 10 s pong
 * timeout and jitters it 0.8–1.0×, so a dead flow surfaces within ~70 s. */
#define REG_KEEPALIVE  60

/* Methods we actually implement. libre answers anything else 501 Not Implemented,
 * which is correct -- but it has no notion of an Allow header, so we supply one
 * (RFC 3261 §20.5: SHOULD be present in INVITE and in 2xx to INVITE). REFER and
 * PRACK are deliberately absent: we pass no refer handler and run REL100_DISABLED. */
#define SIP_ALLOW "INVITE, ACK, CANCEL, BYE, OPTIONS, INFO, UPDATE"

/* User-Agent (RFC 3261 §20.41: `product [SLASH version]`). The old value,
 * "M Keypad SIP/2.0", parsed as three products -- one of them a product literally
 * named SIP at version 2.0, i.e. the protocol version masquerading as our
 * software version. Nothing keys off this string; it exists to be readable in a
 * trace. Kept identical to the ESP build so the fleet looks like one product. */
#define SIP_USER_AGENT "MMKeypad/1.0"
/* Our SIP listening port, and the port advertised in REGISTER's Contact. This
 * MUST be a port we actually listen on, or inbound INVITEs are routed nowhere
 * (see sip_stack_build). 0 would restore libre's default behaviour, which is
 * exactly the bug: it advertises the connection's ephemeral source port.
 *
 * This briefly looked like it broke registration. It does not -- a packet
 * capture on the panel showed the registrar answering 200 OK and echoing the
 * :5060 contact. The registration failures were the registrar's own state (and,
 * once, testing against a FreeSWITCH that had restarted seconds earlier); the
 * EADDRINUSE wedge was a rebuild racing the previous socket, now handled with a
 * fallback rather than a fixed port being wrong. */
#define SIP_LOCAL_PORT 5060
#define RTP_PORT_MIN   16384
#define RTP_PORT_MAX   17384

/* ── state (all touched only on the libre thread unless noted) ────────────── */
static pthread_t        s_thread;
static bool             s_inited;          /* sip_init() ran */
static struct mqueue   *s_mq;              /* other threads -> libre thread */

static struct sip         *s_sip;
static struct sip_lsnr    *s_lsnr;         /* OPTIONS responder */
static struct sipsess_sock *s_sess_sock;
static struct sipreg      *s_reg;
static struct sip_keepalive *s_ka;         /* CRLF keepalive on the registrar flow */
static struct sipsess     *s_sess;         /* the one active call, if any */
static struct sdp_session *s_sdp;
static struct sdp_media   *s_sdp_media;
static struct rtp_sock    *s_rtp;
static struct tmr          s_tmr_tx;       /* 20 ms RTP transmit tick */

static struct sa  s_laddr;                 /* our IP, for SDP + transports */
static struct sa  s_rtp_peer;              /* remote RTP destination */
static bool       s_have_peer;
static uint32_t   s_ts;                    /* RTP timestamp */
static bool       s_marker;                /* set the marker bit on the next packet */
static bool       s_registered;
static uint16_t   s_bound_port;            /* port our SIP transports bound to */
static bool       s_in_call;
static bool       s_ringing;               /* inbound call not yet answered */
static bool       s_inbound;               /* the current call arrived as an INVITE */
/* Negotiated media direction, from OUR point of view. libre stores the remote
 * direction inverted -- a remote "a=recvonly" becomes SDP_SENDONLY -- so the
 * combined ldir&rdir reads directly as "we may send" / "we may receive", and
 * hold (a=sendonly / a=inactive, RFC 3264 §6.1) falls out of the negotiation
 * instead of needing a special case. */
static bool       s_may_send = true;
static bool       s_may_recv = true;
static char       s_peer_uri[160];   /* peer AOR -- used for door matching + reporting */
/* Display names. s_peer_uri MUST stay the AOR: peer_is_doorstation() parses it and
 * LAST_DOOR_PEER reports it. The name is what the UI shows. */
static char       s_peer_name[80];
static char       s_reg_name[80];    /* our own, from the driver's `sip` message */

/* Behaviour, settable from the driver via `callcfg`. */
static bool s_auto_answer = true;
static bool s_monitor;
static bool s_muted;
static bool s_play_door_chime = true;
static char *s_doorstations;               /* "|user|user|…" or NULL */

static sip_call_cb_t s_call_cb;

/* Provisioned credentials (driver-pushed via the `sip` message). The intercom
 * driver auto-generates these -- username MMKeypad_<c4 device id> plus a
 * persisted random password -- and reports them to Director, so nothing is typed
 * by a dealer. See PHASE3-INTERCOM-SIP.md. */
static char s_reg_user[64], s_reg_pass[64], s_reg_host[96];
static int  s_reg_port = 5060;

/* ── reporting back to the driver over net.c's :6700 socket ───────────────── */
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
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "callstate");
    cJSON_AddStringToObject(o, "event", event);
    if (s_peer_uri[0]) cJSON_AddStringToObject(o, "remoteAor", s_peer_uri);
    send_obj(o);
}

static void ui_call_event(const char *event)
{
    /* Show the display name when we have one -- a call used to announce itself as
     * "MMKeypad_3414" because only the AOR was ever passed up. */
    if (s_call_cb) s_call_cb(event, s_peer_name[0] ? s_peer_name : s_peer_uri);
}

/* ── door-station classification ──────────────────────────────────────────── */
/* The driver resolves which endpoints are door stations (Control4's own
 * isDoorStation capability) and pushes their SIP usernames as "|a|b|…". A call
 * from one is a doorbell: manual answer, never auto-answered or monitored. No
 * peer-name heuristics. */
static bool peer_is_doorstation(const char *peer)
{
    if (!peer || !peer[0] || !s_doorstations) return false;
    char user[80];
    size_t i = 0;
    const char *p = peer;
    if (!strncmp(p, "sip:", 4)) p += 4;
    for (; p[i] && p[i] != '@' && i + 1 < sizeof(user); i++) user[i] = p[i];
    user[i] = '\0';
    char needle[84];
    re_snprintf(needle, sizeof(needle), "|%s|", user);
    return strstr(s_doorstations, needle) != NULL;
}

/* ── RTP ──────────────────────────────────────────────────────────────────── */
/* Receive: µ-law straight from the wire into the speaker. audio_call_write does
 * the decode + 8k->48k resample (see audio_linux.c). */
static void rtp_recv(const struct sa *src, const struct rtp_header *hdr,
                     struct mbuf *mb, void *arg)
{
    (void)src; (void)arg;
    if (!s_in_call || !s_may_recv || hdr->pt != PT_PCMU) return;
    size_t n = mbuf_get_left(mb);
    if (n) audio_call_write(mbuf_buf(mb), (int)n);
}

/* RTCP receive. libre has already folded the report into its own session stats
 * by the time we see it; we only listen so that rtp_listen() can run with RTCP
 * enabled, which is what makes libre EMIT the periodic SR/RR that RFC 3550 §6
 * requires of every RTP sender. */
static void rtcp_recv(const struct sa *src, struct rtcp_msg *msg, void *arg)
{
    (void)src; (void)msg; (void)arg;
}

/* Transmit tick: one 20 ms packet of mic audio. audio_call_read does the
 * 48k->8k resample + µ-law encode. */
static void tx_tick(void *arg)
{
    (void)arg;
    tmr_start(&s_tmr_tx, PTIME_MS, tx_tick, NULL);
    /* !s_may_send is the far end holding us (a=recvonly/inactive): stop sending
     * outright rather than streaming silence at a peer that asked for nothing. */
    if (!s_in_call || !s_rtp || !s_have_peer || !s_may_send) return;

    uint8_t ulaw[SAMPLES_PER_PKT];
    int n = s_muted ? 0 : audio_call_read(ulaw, sizeof(ulaw));
    if (n <= 0) {
        /* Muted, or the mic hiccuped: send silence so the far end's jitter
         * buffer and media timeout keep seeing a live stream. */
        memset(ulaw, 0xFF, sizeof(ulaw));   /* 0xFF = µ-law silence */
        n = sizeof(ulaw);
    }

    struct mbuf *mb = mbuf_alloc(RTP_HEADER_SIZE + n);
    if (!mb) return;
    mb->pos = RTP_HEADER_SIZE;
    mbuf_write_mem(mb, ulaw, n);
    mb->pos = RTP_HEADER_SIZE;
    /* Marker on the first packet of the stream (RFC 3551 §4.1) so the far end
     * knows to (re)prime its jitter buffer rather than treating us as a gap. */
    (void)rtp_send(s_rtp, &s_rtp_peer, false, s_marker, PT_PCMU, s_ts,
                   tmr_jiffies_rt_usec(), mb);
    s_marker = false;
    mem_deref(mb);
    s_ts += n;
}

/* ── SDP ──────────────────────────────────────────────────────────────────── */
static int sdp_setup(void)
{
    int err;

    s_sdp = mem_deref(s_sdp);
    s_sdp_media = NULL;

    err = sdp_session_alloc(&s_sdp, &s_laddr);
    if (err) return err;

    err = sdp_media_add(&s_sdp_media, s_sdp, "audio",
                        sa_port(rtp_local(s_rtp)), sdp_proto_rtpavp);
    if (err) return err;

    /* Offer PCMU only. Control4 intercom is G.711 µ-law; advertising more just
     * invites a codec we cannot encode. */
    err = sdp_format_add(NULL, s_sdp_media, false, "0", "PCMU", RTP_RATE, 1,
                         NULL, NULL, NULL, false, NULL);
    if (err) return err;

    sdp_media_set_lattr(s_sdp_media, true, "ptime", "%u", PTIME_MS);
    sdp_media_set_ldir(s_sdp_media, SDP_SENDRECV);
    s_may_send = s_may_recv = true;
    return 0;
}

/* Pull the remote RTP address and the negotiated direction out of the SDP. Runs
 * for the initial exchange and for every re-INVITE/UPDATE, so a hold and a media
 * re-target both land here. */
static void sdp_apply_remote(void)
{
    if (!s_sdp_media) return;
    const struct sa *rem = sdp_media_raddr(s_sdp_media);
    if (sa_isset(rem, SA_ALL)) {
        s_rtp_peer = *rem;
        s_have_peer = true;
    }

    enum sdp_dir dir = sdp_media_dir(s_sdp_media);
    bool send = (dir & SDP_SENDONLY) != 0;
    if (send && !s_may_send) s_marker = true;   /* resuming: new talkspurt */
    s_may_send = send;
    s_may_recv = (dir & SDP_RECVONLY) != 0;
}

/* ── call lifecycle ───────────────────────────────────────────────────────── */
/* Our RTCP CNAME (RFC 3550 §6.5.1: a stable, globally unique identifier for this
 * participant). Our AOR is exactly that. */
static const char *rtcp_cname(void)
{
    static char cname[160];
    re_snprintf(cname, sizeof(cname), "sip:%s@%s",
                s_reg_user[0] ? s_reg_user : "mmkeypad",
                s_reg_host[0] ? s_reg_host : "localhost");
    return cname;
}

static void call_teardown(const char *event)
{
    tmr_cancel(&s_tmr_tx);
    if (s_in_call) {
        audio_call_end();
        /* Tell the far end this stream is over (RFC 3550 §6.3.7 RTCP BYE), then
         * stop the reporting timer -- the socket outlives the call. */
        (void)rtcp_send_bye_packet(s_rtp);
        rtcp_start(s_rtp, rtcp_cname(), NULL);
    }
    if (s_ringing) { audio_ring_stop(); audio_amp(false); }
    s_in_call = s_ringing = s_have_peer = s_inbound = false;
    s_peer_name[0] = '\0';
    s_sess = mem_deref(s_sess);
    if (event) { send_callstate(event); ui_call_event(event); }
}

static void call_established(void)
{
    if (s_ringing) { audio_ring_stop(); s_ringing = false; }
    sdp_apply_remote();
    audio_call_begin(RTP_RATE);
    s_in_call = true;
    /* Random starting timestamp, so an observer cannot infer how long this UA has
     * been up or correlate streams across calls (RFC 3550 §5.1). The SSRC and
     * sequence number libre randomises for us. */
    s_ts = rand_u32();
    s_marker = true;
    (void)rtp_clear(s_rtp);          /* drop anything queued from a prior call */

    struct sa rtcp_peer;
    sa_init(&rtcp_peer, AF_INET);
    sdp_media_raddr_rtcp(s_sdp_media, &rtcp_peer);   /* a=rtcp, else RTP port + 1 */
    if (sa_isset(&rtcp_peer, SA_ALL))
        rtcp_start(s_rtp, rtcp_cname(), &rtcp_peer);

    tmr_start(&s_tmr_tx, PTIME_MS, tx_tick, NULL);
    send_callstate("accepted");
    ui_call_event("active");
}

static void sess_estab(const struct sip_msg *msg, void *arg)
{
    (void)msg; (void)arg;
    call_established();
}

static void sess_close(int err, const struct sip_msg *msg, void *arg)
{
    (void)arg;
    /* libre signals a NORMALLY RECEIVED BYE as ECONNRESET with no message (it
     * has already answered it 200 OK -- see bye_handler in sipsess/listen.c).
     * Reporting that verbatim made a clean hangup read as a transport failure
     * and cost real debugging time, so name the ordinary case. */
    if (err == ECONNRESET && !msg)
        re_printf("sip: call closed (peer hung up)\n");
    else if (err)
        re_printf("sip: call closed (error %d: %s)\n", err, strerror(err));
    else
        re_printf("sip: call closed (scode=%u)\n", msg ? msg->scode : 0);
    call_teardown("ended");
}

/* Remote sent us an offer (inbound INVITE, or a re-INVITE). */
static int sess_offer(struct mbuf **descp, const struct sip_msg *msg, void *arg)
{
    (void)arg;
    if (msg && mbuf_get_left(msg->mb))
        (void)sdp_decode(s_sdp, msg->mb, true);
    sdp_apply_remote();
    return sdp_encode(descp, s_sdp, false);   /* our answer */
}

/* Remote answered our offer (outbound call). */
static int sess_answer(const struct sip_msg *msg, void *arg)
{
    (void)arg;
    if (msg && mbuf_get_left(msg->mb))
        (void)sdp_decode(s_sdp, msg->mb, false);
    sdp_apply_remote();
    return 0;
}

/* Inbound INVITE. */
static void sess_conn(const struct sip_msg *msg, void *arg)
{
    (void)arg;

    if (s_sess) {                       /* already busy -> 486 */
        (void)sip_treply(NULL, s_sip, msg, 486, "Busy Here");
        return;
    }
    if (sdp_setup()) {
        (void)sip_treply(NULL, s_sip, msg, 500, "Server Internal Error");
        return;
    }
    if (mbuf_get_left(msg->mb))
        (void)sdp_decode(s_sdp, msg->mb, true);
    sdp_apply_remote();

    re_snprintf(s_peer_uri, sizeof(s_peer_uri), "%r@%r",
                &msg->from.uri.user, &msg->from.uri.host);
    /* Prefer the caller's display name ("Kitchen") over the AOR. */
    if (pl_isset(&msg->from.dname))
        re_snprintf(s_peer_name, sizeof(s_peer_name), "%r", &msg->from.dname);
    else
        s_peer_name[0] = '\0';

    bool is_door = peer_is_doorstation(s_peer_uri);
    re_printf("sip: incoming from %s (autoAnswer=%d monitor=%d door=%d)\n",
              s_peer_uri, s_auto_answer, s_monitor, is_door);

    struct mbuf *desc = NULL;
    if (sdp_encode(&desc, s_sdp, false)) {
        (void)sip_treply(NULL, s_sip, msg, 500, "Server Internal Error");
        return;
    }

    int err = sipsess_accept(&s_sess, s_sess_sock, msg, 180, "Ringing",
                             REL100_DISABLED, "mmkeypad", "application/sdp", NULL,
                             NULL, NULL, false,
                             sess_offer, sess_answer, sess_estab,
                             NULL, NULL, sess_close, NULL,
                             "Allow: " SIP_ALLOW "\r\n");
    if (err) {
        mem_deref(desc);
        (void)sip_treply(NULL, s_sip, msg, 500, "Server Internal Error");
        return;
    }

    s_inbound = true;
    send_callstate("incoming");

    /* Door stations always ring for a human. Monitor mode picks up silently.
     * Otherwise auto-answer (announcements) or ring for the on-screen UI. */
    if (is_door) {
        ui_call_event("incoming");
        audio_ring_start_ex(s_play_door_chime);
        s_ringing = true;
    } else if (s_monitor) {
        (void)sipsess_answer(s_sess, 200, "OK", desc, "Allow: " SIP_ALLOW "\r\n");
        desc = NULL;
    } else if (s_auto_answer) {
        audio_play_beep();                      /* heads-up, before the codec flips */
        (void)sipsess_answer(s_sess, 200, "OK", desc, "Allow: " SIP_ALLOW "\r\n");
        desc = NULL;
    } else {
        ui_call_event("incoming");
        audio_ring_start();
        s_ringing = true;
    }
    mem_deref(desc);
}

static void do_answer(void)
{
    if (!s_sess || s_in_call) return;
    struct mbuf *desc = NULL;
    if (sdp_encode(&desc, s_sdp, false)) return;
    if (s_ringing) { audio_ring_stop(); s_ringing = false; }
    (void)sipsess_answer(s_sess, 200, "OK", desc, "Allow: " SIP_ALLOW "\r\n");
    mem_deref(desc);
}

static void do_hangup(void)
{
    if (!s_sess) return;

    /* Declining a call that is still ringing is not the same as hanging one up.
     * mem_deref alone lets libre's session destructor answer 486 Busy Here, which
     * tells the caller (and Control4's own call log) that the panel was on another
     * call. 603 Decline is what "the human said no" means -- RFC 3261 §21.6.2.
     * Only valid while the INVITE's server transaction is still open, i.e. exactly
     * while we are ringing an inbound call; after this the transaction is complete
     * and the deref below cannot double-answer it (sip_strans_reply clears sess->st
     * on any final response). Outbound calls fall through to the deref, where libre
     * CANCELs a still-ringing INVITE and BYEs an established one -- both correct. */
    if (s_inbound && s_ringing && !s_in_call)
        (void)sipsess_reject(s_sess, 603, "Decline", NULL);

    call_teardown("ended");
}

/* This libre's sipsess_connect() takes NO `desc` argument: it rebuilds the request
 * body on every (re)transmit -- including the retry after a 401/407 challenge --
 * by calling the desc handler. Passing NULL sent an INVITE that advertised
 * Content-Type: application/sdp with no body, so the offer never reached the far
 * end. Encoding into a local mbuf (and then just mem_deref'ing it) did nothing. */
static int sess_desc(struct mbuf **descp, const struct sa *src,
                     const struct sa *dst, void *arg)
{
    (void)src; (void)dst; (void)arg;
    return sdp_encode(descp, s_sdp, true);   /* our offer */
}

static int auth_handler(char **user, char **pass, const char *realm, void *arg);

static void do_place_call(const char *arg)
{
    /* "user\tdname" -- see sip_place_call(). */
    char user[120] = {0};
    const char *tab = arg ? strchr(arg, '\t') : NULL;
    if (tab) {
        size_t n = (size_t)(tab - arg);
        if (n >= sizeof(user)) n = sizeof(user) - 1;
        memcpy(user, arg, n);
        re_snprintf(s_peer_name, sizeof(s_peer_name), "%s", tab + 1);
    } else if (arg) {
        re_snprintf(user, sizeof(user), "%s", arg);
        s_peer_name[0] = '\0';
    }
    if (s_sess || !user[0] || !s_sip) return;
    if (sdp_setup()) return;

    char to_uri[192], from_uri[192];
    re_snprintf(to_uri, sizeof(to_uri), "sip:%s@%s", user, s_reg_host);
    re_snprintf(from_uri, sizeof(from_uri), "sip:%s@%s", s_reg_user, s_reg_host);

    /* auth_handler is REQUIRED here: the Director's FreeSWITCH challenges INVITE
     * (not just REGISTER). With no handler the challenge went unanswered and the
     * channel sat in CS_NEW until FreeSWITCH abandoned it ~10s later
     * ("Abandoned ... [CS_NEW] [WRONG_CALL_STATE]"), which is why outbound
     * intercom calls from the T3 silently failed while registration succeeded. */
    int err = sipsess_connect(&s_sess, s_sess_sock, to_uri,
                              s_reg_name[0] ? s_reg_name : NULL, from_uri,
                              "mmkeypad", NULL, 0, "application/sdp",
                              auth_handler, NULL, false, NULL, sess_desc,
                              sess_offer, sess_answer, NULL, sess_estab,
                              NULL, NULL, sess_close, NULL,
                              "Allow: " SIP_ALLOW "\r\n");
    if (err) { re_printf("sip: outbound call failed: %d\n", err); return; }

    re_snprintf(s_peer_uri, sizeof(s_peer_uri), "%s", user);
    /* Local ringback while the far end rings. s_ringing is what
     * call_established()/call_teardown() key off to stop it, so answering or
     * hanging up silences it without any extra plumbing. */
    s_ringing = true;
    audio_ringback_start();
    send_callstate("outgoing");
    ui_call_event("outgoing");
}


/* ── registration ─────────────────────────────────────────────────────────── */
static int auth_handler(char **user, char **pass, const char *realm, void *arg)
{
    (void)realm; (void)arg;
    int err = str_dup(user, s_reg_user);
    err |= str_dup(pass, s_reg_pass);
    return err;
}

static void do_register(void);

/* The registrar's flow went quiet: libre sent a double-CRLF ping and got nothing
 * back inside 10 s, so it has already closed that connection and dropped it from
 * its pool. Registering again therefore dials a FRESH TCP connection -- which is
 * the whole point, since inbound INVITEs ride the connection we opened. */
static void ka_handler(int err, void *arg)
{
    (void)arg;                   /* libre already freed the keepalive and NULLed s_ka */
    if (s_registered) {
        s_registered = false;
        send_sipstate(false);
    }
    re_printf("sip: registrar flow died (%s) -- re-registering\n", strerror(err));
    do_register();
}

static void reg_handler(int err, const struct sip_msg *msg, void *arg)
{
    (void)arg;
    bool ok = (!err && msg && msg->scode >= 200 && msg->scode < 300);
    if (ok != s_registered) {
        s_registered = ok;
        send_sipstate(ok);
    }
    if (err)
        re_printf("sip: register failed: %s\n", strerror(err));
    else if (msg)
        re_printf("sip: register -> %u %r (expires %us)\n", msg->scode,
                  &msg->reason, sipreg_proxy_expires(s_reg));

    /* Watch the flow we just registered over. Only once: the keepalive lives on
     * libre's connection object (started on the first success, cleared by
     * ka_handler when that connection dies), so re-arming it on every refresh
     * would just pile up handler objects on the same socket. */
    /* TCP/TLS only. On UDP libre keeps the flow alive with STUN binding requests
     * (RFC 5626 §4.4.2) instead, and a registrar that answers SIP but not STUN
     * would read as a dead flow every minute. We always register over TCP (see
     * do_register), so this is a guard, not a code path. */
    if (ok && !s_ka && msg &&
        (msg->tp == SIP_TRANSP_TCP || msg->tp == SIP_TRANSP_TLS))
        (void)sip_keepalive_start(&s_ka, s_sip, msg, REG_KEEPALIVE,
                                  ka_handler, NULL);
}

static void do_register(void)
{
    if (!s_sip || !s_reg_user[0] || !s_reg_host[0]) return;

    s_ka  = mem_deref(s_ka);
    s_reg = mem_deref(s_reg);

    char reg_uri[160], aor[192];
    /* ;transport=tcp is not optional here -- see the file header. */
    re_snprintf(reg_uri, sizeof(reg_uri), "sip:%s:%d;transport=tcp",
                s_reg_host, s_reg_port);
    re_snprintf(aor, sizeof(aor), "sip:%s@%s", s_reg_user, s_reg_host);

    /* alloc + set contact port + send, so the FIRST REGISTER already advertises
     * the right Contact rather than relying on the authenticated retry to carry
     * it. */
    int err = sipreg_alloc(&s_reg, s_sip, reg_uri, aor, NULL, aor,
                           REG_EXPIRES, s_reg_user, NULL, 0, 0,
                           auth_handler, NULL, false,
                           reg_handler, NULL, NULL, NULL);
    if (!err) {
        if (s_bound_port) sipreg_set_contact_port(s_reg, s_bound_port);
        err = sipreg_send(s_reg);
    }
    if (err) {
        s_reg = mem_deref(s_reg);
        re_printf("sip: sipreg_register failed: %d (%s)\n", err, strerror(err));
        return;
    }

    re_printf("sip: registering %s via %s (contact port %u)\n",
              aor, reg_uri, s_bound_port);
}

/* RFC 3261 §11.2: a UAS MUST answer OPTIONS, and the answer SHOULD carry the same
 * status an INVITE would get right now plus the capability headers. libre has no
 * built-in responder, so an OPTIONS drops through to its catch-all 501 Not
 * Implemented -- legal as a "something is alive here", but it tells a registrar
 * that pings its contacts (FreeSWITCH sofia does) nothing about what we support,
 * and reads as a broken endpoint in a trace. Runs before sipsess's own listener
 * and returns false for everything else, so INVITE/ACK/BYE/… are untouched. */
static bool options_handler(const struct sip_msg *msg, void *arg)
{
    (void)arg;
    if (pl_strcmp(&msg->met, "OPTIONS"))
        return false;

    (void)sip_treplyf(NULL, NULL, s_sip, msg, false,
                      s_sess ? 486 : 200, s_sess ? "Busy Here" : "OK",
                      "Allow: " SIP_ALLOW "\r\n"
                      "Accept: application/sdp\r\n"
                      "Accept-Language: en\r\n"
                      "Content-Length: 0\r\n"
                      "\r\n");
    return true;
}

/* Allocate the SIP stack + transports + session listener. */
static int transports_add(uint16_t port)
{
    struct sa taddr = s_laddr;
    sa_set_port(&taddr, port);

    /* TCP is the transport. Inbound INVITEs ride it and we always register
     * ;transport=tcp, so this one failing is the only real failure. */
    if (sip_transp_add(s_sip, SIP_TRANSP_TCP, &taddr)) return EADDRINUSE;

    /* UDP is a courtesy: it exists so a UDP-only registrar could work, and
     * nothing we do today uses it. It is also the socket that LOSES the restart
     * race -- libre sets SO_REUSEADDR/SO_REUSEPORT on its TCP listeners but not
     * on its UDP ones (net_sockopt_reuse_set is called from tcp.c's listen path,
     * not udp.c's), so a lingering UDP :5060 from the previous process is the
     * usual EADDRINUSE here. Treating that as fatal is what used to condemn the
     * whole bind and demote us to an ephemeral Contact -- throwing away a TCP
     * :5060 that had bound perfectly well, to punish a socket we never send on.
     * Log it and carry on. */
    if (sip_transp_add(s_sip, SIP_TRANSP_UDP, &taddr))
        re_printf("sip: UDP port %u unavailable -- TCP only (all we use anyway)\n",
                  port);
    return 0;
}

static int sip_stack_build(void)
{
    if (sip_alloc(&s_sip, NULL, 32, 32, 32, SIP_USER_AGENT, NULL, NULL)) {
        re_printf("sip: sip_alloc failed\n");
        return EINVAL;
    }

    /* Bind to a FIXED port. With port 0 libre picked an ephemeral LISTENING port
     * (e.g. 52173) while REGISTER's Contact advertised the ephemeral SOURCE port
     * of the outbound connection (send_handler in libre's sipreg/reg.c does
     * "reg->laddr = *src"). Two different ports -- so the registrar routed
     * INVITEs at a port nothing listened on, and at a STALE one after every
     * reconnect (observed walking 50577 -> 50592 -> 50616). That is why inbound
     * calls died before the panel ever saw them. Verified on the wire: with the
     * fixed port, FreeSWITCH answers 200 OK echoing
     * "Contact: <sip:MMKeypad_3414@192.168.174.206:5060>;expires=<granted>". */
    /* No retry loop any more. It slept up to 5 s at startup waiting out a
     * lingering listener, but the TCP socket never needed waiting for --
     * libre binds it with SO_REUSEADDR/SO_REUSEPORT, so it rebinds through
     * TIME_WAIT immediately. The wait was really being spent on the UDP socket,
     * which transports_add() no longer treats as fatal. */
    s_bound_port = SIP_LOCAL_PORT;
    if (transports_add(s_bound_port)) {
        /* Something else genuinely holds TCP 5060 (another instance of us).
         * Fall back to an ephemeral port rather than wedge: a panel that can
         * register is worth more than one that cannot. */
        re_printf("sip: TCP port %u unavailable -- falling back to an ephemeral port\n",
                  s_bound_port);
        sip_close(s_sip, true);
        s_sip = mem_deref(s_sip);
        if (sip_alloc(&s_sip, NULL, 32, 32, 32, SIP_USER_AGENT, NULL, NULL)) {
            re_printf("sip: sip_alloc failed\n");
            return EINVAL;
        }
        s_bound_port = 0;
        if (transports_add(0)) {
            re_printf("sip: transport add failed\n");
            return EINVAL;
        }
        /* ...and then ADVERTISE the port we actually got. This is the half that
         * was missing: the old fallback left s_bound_port at 0, so nothing called
         * sipreg_set_contact_port and the Contact reverted to libre's default --
         * the connection's ephemeral SOURCE port, which is a different number from
         * the ephemeral port we LISTEN on and changes on every reconnect. That is
         * the original inbound-is-dead bug, re-entered through the back door of
         * the very fallback meant to be the safe option. libre records each
         * transport's real bound address after listen(), so we can just ask. */
        struct sa la;
        if (!sip_transp_laddr(s_sip, &la, SIP_TRANSP_TCP, &s_laddr))
            s_bound_port = sa_port(&la);
    }

    if (sip_listen(&s_lsnr, s_sip, true, options_handler, NULL)) {
        re_printf("sip: sip_listen failed\n");
        return EINVAL;
    }
    if (sipsess_listen(&s_sess_sock, s_sip, 32, sess_conn, NULL)) {
        re_printf("sip: sipsess_listen failed\n");
        return EINVAL;
    }
    return 0;
}

/* ── cross-thread command queue ───────────────────────────────────────────── */
/* net.c and the UI run on other threads; libre must only be touched from its own
 * loop. Commands are copied and pushed here, then executed on the libre thread. */
enum cmd { CMD_REGISTER = 1, CMD_ANSWER, CMD_HANGUP, CMD_CALL, CMD_QUIT };

static void mq_handler(int id, void *data, void *arg)
{
    (void)arg;
    switch (id) {
    case CMD_REGISTER: do_register();               break;
    case CMD_ANSWER:   do_answer();                 break;
    case CMD_HANGUP:   do_hangup();                 break;
    case CMD_CALL:     do_place_call((char *)data); break;
    case CMD_QUIT:     re_cancel();                 break;
    }
    mem_deref(data);
}

static void post(enum cmd id, const char *arg)
{
    if (!s_mq) return;
    char *copy = NULL;
    if (arg) str_dup(&copy, arg);
    if (mqueue_push(s_mq, (int)id, copy)) mem_deref(copy);
}

/* ── libre event loop ─────────────────────────────────────────────────────── */
static void *sip_thread(void *arg)
{
    (void)arg;
    if (libre_init()) { re_printf("sip: libre_init failed\n"); return NULL; }

    /* Bind to whatever address the default route uses; SDP and the SIP
     * transports both need a real (non-loopback) address. */
    if (net_default_source_addr_get(AF_INET, &s_laddr)) {
        re_printf("sip: no usable local address\n");
        goto out;
    }
    sa_set_port(&s_laddr, 0);

    if (sip_stack_build()) goto out;

    /* RTCP enabled: libre binds it on the next (odd) port, which is where the far
     * end expects it (RFC 3550 §11), and starts emitting the sender/receiver
     * reports RFC 3550 §6 requires once rtcp_start() names a peer. */
    if (rtp_listen(&s_rtp, IPPROTO_UDP, &s_laddr, RTP_PORT_MIN, RTP_PORT_MAX,
                   true, rtp_recv, rtcp_recv, NULL)) {
        re_printf("sip: rtp_listen failed\n");
        goto out;
    }
    if (mqueue_alloc(&s_mq, mq_handler, NULL)) {
        re_printf("sip: mqueue_alloc failed\n");
        goto out;
    }

    tmr_init(&s_tmr_tx);
    re_printf("sip: ready on %j (RTP %u), control over :6700\n",
              &s_laddr, sa_port(rtp_local(s_rtp)));

    re_main(NULL);

out:
    tmr_cancel(&s_tmr_tx);
    s_ka        = mem_deref(s_ka);
    s_reg       = mem_deref(s_reg);
    s_sess      = mem_deref(s_sess);
    s_sess_sock = mem_deref(s_sess_sock);
    s_lsnr      = mem_deref(s_lsnr);
    s_rtp       = mem_deref(s_rtp);
    s_sdp       = mem_deref(s_sdp);
    s_mq        = mem_deref(s_mq);
    s_sip       = mem_deref(s_sip);
    libre_close();
    return NULL;
}

/* ── public API (sip.h) ───────────────────────────────────────────────────── */
void sip_init(void)
{
    if (s_inited) return;
    if (pthread_create(&s_thread, NULL, sip_thread, NULL)) {
        re_printf("sip: cannot start SIP thread\n");
        return;
    }
    pthread_detach(s_thread);
    s_inited = true;
}

void sip_set_call_cb(sip_call_cb_t cb) { s_call_cb = cb; }
// Mic mute for the live call, driven by the on-screen Mute button. Same flag the
// driver's MUTE_CALL sets via callcfg, so both paths agree (see the read side in
// the RTP send loop, which sends silence while muted).
void sip_set_mute(bool on)             { s_muted = on; }
bool sip_is_muted(void)                { return s_muted; }

void sip_answer(void)                  { post(CMD_ANSWER, NULL); }
void sip_hangup(void)                  { post(CMD_HANGUP, NULL); }
/* Pack "user\tdname" through the mqueue: post() carries one string, and the two
 * must arrive together or the call screen races the name. */
void sip_place_call(const char *user, const char *dname)
{
    char buf[200];
    re_snprintf(buf, sizeof(buf), "%s\t%s", user ? user : "", dname ? dname : "");
    post(CMD_CALL, buf);
}

void sip_set_doorstations(const char *delimited)
{
    free(s_doorstations);
    s_doorstations = (delimited && delimited[0]) ? strdup(delimited) : NULL;
    re_printf("sip: door stations: %s\n", s_doorstations ? s_doorstations : "(none)");
}

static const char *jstr(const cJSON *o, const char *k, const char *def)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : def;
}

void sip_handle(const cJSON *d)
{
    if (!s_inited || !d) return;
    const char *t = jstr(d, "t", "");

    if (!strcmp(t, "sip")) {
        const char *server = jstr(d, "server", NULL);
        const char *user   = jstr(d, "user", NULL);
        if (!server || !user) { re_printf("sip: creds missing server/user\n"); return; }
        const cJSON *p    = cJSON_GetObjectItem(d, "port");
        const cJSON *aa   = cJSON_GetObjectItem(d, "autoAnswer");
        const char  *pass = jstr(d, "pass", "");
        int          port = cJSON_IsNumber(p) ? p->valueint : 5060;

        /* The driver re-pushes these on every link-up. Re-registering because the
         * TCP to the DRIVER blipped tears down a working SIP registration (and,
         * with it, the flow inbound INVITEs ride) for no reason -- and de-registers
         * then re-registers us at the registrar mid-call. Only act on a change.
         * Display name and auto-answer are local behaviour, so they apply either
         * way; only the credentials trigger a REGISTER. */
        bool changed = strcmp(s_reg_host, server) || strcmp(s_reg_user, user) ||
                       strcmp(s_reg_pass, pass)   || s_reg_port != port;
        re_snprintf(s_reg_host, sizeof(s_reg_host), "%s", server);
        re_snprintf(s_reg_user, sizeof(s_reg_user), "%s", user);
        re_snprintf(s_reg_pass, sizeof(s_reg_pass), "%s", pass);
        re_snprintf(s_reg_name, sizeof(s_reg_name), "%s", jstr(d, "name", ""));
        s_reg_port = port;
        if (cJSON_IsBool(aa)) s_auto_answer = cJSON_IsTrue(aa);
        if (changed || !s_registered) post(CMD_REGISTER, NULL);
        else re_printf("sip: creds unchanged -- keeping the current registration\n");

    } else if (!strcmp(t, "callcfg")) {
        /* Live behaviour from the intercom proxy; does NOT re-register. */
        const cJSON *aa  = cJSON_GetObjectItem(d, "autoAnswer");
        const cJSON *mon = cJSON_GetObjectItem(d, "monitor");
        const cJSON *mut = cJSON_GetObjectItem(d, "mute");
        const cJSON *pdc = cJSON_GetObjectItem(d, "playDoorChime");
        if (cJSON_IsBool(aa))  s_auto_answer     = cJSON_IsTrue(aa);
        if (cJSON_IsBool(mon)) s_monitor         = cJSON_IsTrue(mon);
        if (cJSON_IsBool(mut)) s_muted           = cJSON_IsTrue(mut);
        if (cJSON_IsBool(pdc)) s_play_door_chime = cJSON_IsTrue(pdc);

    } else if (!strcmp(t, "call")) {
        const char *a = jstr(d, "action", "");
        if      (!strcmp(a, "accept")) post(CMD_ANSWER, NULL);
        else if (!strcmp(a, "start"))  post(CMD_CALL, jstr(d, "remote", ""));
        else if (!strcmp(a, "end") || !strcmp(a, "reject")) post(CMD_HANGUP, NULL);
    }
    /* unknown types ignored (forward-compat) */
}

#endif /* MMK_HAS_SIP */
