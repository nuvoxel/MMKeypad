#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "board.h"   // MMK_HAS_AUDIO / MMK_HAS_SIP — SIP-less boards get no-op stubs below

// SIP defaults to following the audio flag (ESP audio boards get SIP too), but a
// board can define MMK_HAS_SIP 0 to have local audio WITHOUT the SIP UA — e.g. the
// T3 Linux target, which brings up ALSA audio before its (baresip) SIP stack lands.
#ifndef MMK_HAS_SIP
#define MMK_HAS_SIP MMK_HAS_AUDIO
#endif

// Forward decl so callers don't have to pull in cJSON just to route a message.
typedef struct cJSON cJSON;

// ── Phase 3 intercom: SIP user agent (esp_rtc) ──────────────────────────────
// The SIP control protocol is multiplexed onto the SAME :6700 socket as the
// now-playing/keypad protocol (net.c) — there is no separate TCP channel, so the
// keypad driver's single network binding serves intercom too (one Composer bind).
//
// The driver pushes SIP credentials (a `sip` message) → we register with the SIP
// server (Director) via esp_rtc; inbound INVITEs become calls whose audio flows
// through the ES8311 (audio.c). We report `sipstate` / `callstate` back over the
// same link (net_send_line), which the driver mirrors onto the Intercom proxy.

// Call-state callback for the on-screen UI. `event` is one of "incoming",
// "outgoing", "active", "ended"; `peer` is the remote party (may be NULL/empty).
// Fired from the esp_rtc/SIP task — the handler must marshal onto the LVGL task
// (e.g. take lvgl_port_lock) before touching the UI. See main.c.
typedef void (*sip_call_cb_t)(const char *event, const char *peer);

#if MMK_HAS_SIP

// Initialise the SIP module (registers the media_lib adapter esp_rtc needs).
// Call once at boot, only when audio is ready. Registration itself happens lazily
// when the driver provisions creds via a `sip` message.
void sip_init(void);

// Handle a `sip` or `call` control message (already parsed) routed from net.c.
// Safe / no-op before sip_init() (e.g. audio failed to come up).
void sip_handle(const cJSON *d);

// ── Local call UI bridge ────────────────────────────────────────────────────
void sip_set_call_cb(sip_call_cb_t cb);

// Answer / hang up the current call from the on-screen buttons. No-op if no call.
void sip_answer(void);
void sip_hangup(void);
// Outbound intercom call to a SIP username. `dname` is the friendly name to show
// on the call screen (may be NULL) -- without it the UI could only fall back to
// whatever the stack reported, which showed the raw AOR on the T3 and a bare
// "intercom" on the ws43.
void sip_place_call(const char *user, const char *dname);

// Mic mute for the CURRENT call, driven by the on-screen Mute button. Same switch
// the driver's MUTE_CALL sets (callcfg.mute) -- the UI just flips it locally and
// reports it up, so both paths agree.
void sip_set_mute(bool on);
bool sip_is_muted(void);

// Set the door-station SIP usernames (driver-pushed, "|user1|user2|…" delimited, or NULL).
// A call whose peer is in this set is treated as a doorbell. Copies the string.
void sip_set_doorstations(const char *delimited);

#else
// Audio-less boards (e.g. crowpanel display-only phases): sip.c isn't compiled,
// but shared callers (ui.c intercom list, net.c message routing, main.c) stay
// unconditional — every entry point is a safe no-op.
static inline void sip_init(void) {}
static inline void sip_handle(const cJSON *d) { (void)d; }
static inline void sip_set_call_cb(sip_call_cb_t cb) { (void)cb; }
static inline void sip_answer(void) {}
static inline void sip_hangup(void) {}
static inline void sip_place_call(const char *user, const char *dname) { (void)user; (void)dname; }
static inline void sip_set_doorstations(const char *delimited) { (void)delimited; }
static inline void sip_set_mute(bool on) { (void)on; }
static inline bool sip_is_muted(void) { return false; }
#endif
