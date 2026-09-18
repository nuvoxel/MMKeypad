#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "board.h"   // MMK_MAX_BUTTONS sizes the inbound button array below

// Wire-protocol version the firmware speaks (PROTOCOL.md). The driver sends its own
// `proto` in `state`; a mismatch is logged but tolerated (fields are additive).
#define NET_PROTO_VERSION 1

// ── Data model (mirrors PROTOCOL.md `state`) ────────────────────────────────
// Fixed-size arrays intentionally cap inbound list growth (review fix: a hostile
// or buggy driver can't drive unbounded heap from a `state`).
#define NET_MAX_META    12
// Follows the board's advertised capacity: we tell the driver caps.buttons =
// MMK_MAX_BUTTONS, so anything less here would drop buttons we asked for.
#define NET_MAX_BUTTONS MMK_MAX_BUTTONS
#define NET_MAX_ENDPOINTS 24
#define NET_MAX_ROOMS   32
#define NET_MAX_FAVORITES 24
// Real thermostats in this house top out at 6 (GET_COMFORT_DEVICES, confirmed live);
// a generous ceiling purely as a sanity bound against a hostile/buggy driver, same
// idea as NET_MAX_FAVORITES.
#define NET_MAX_COMFORT 12

typedef struct { char id[24]; char name[48]; } id_name_t;

// A room favorite tile (driver reply to `getfavorites`, PROTOCOL.md). Room-level
// (no source id). `id` is the opaque favorite id sent back in a `favorite` message to
// activate it; `title` is the display label; `art_url` is the tile artwork URL (may be
// empty); `kind` is `stream` | `broadcast` | `light` | `shade` | `relay` (informational
// — the driver enacts the action; the device just sends the id back). `kind:"comfort"`
// USED to exist here (a read-only per-room thermostat shortcut) but was removed: the
// dedicated Comfort page (comfort_state_t below) replaced it entirely, since comfort
// was never room-scoped in this install and a global menu is the correct model. `on`
// is a generic on/active state, meaningful for `kind:"light"` (on/off) and
// `kind:"shade"` (open/closed, driver's own last-commanded guess — UNVERIFIED, see
// driver.lua BuildShadeFavorites) and always false for stream/broadcast/relay.
typedef struct { char id[40]; char title[96]; char art_url[300]; char kind[12]; bool on; } favorite_t;

// Callable intercom target (driver-pushed): a room/door-station endpoint or a group.
// `user` is the SIP username (endpoint) or group name; calling a group broadcasts.
// `actions` are the endpoint's OWN door actions, reported by the driver -- a door
// station typically has two relays (door + gate), but it may have one or none,
// and only its configuration knows. The panel renders exactly what it is told
// and never infers an action from the target's name or kind. There is no unlock
// command in the Control4 intercom proxy, so the driver turns a press into a
// programming EVENT the installer wires to a relay / lock / macro.
#define NET_MAX_DOOR_ACTIONS 2
typedef struct { char id[16]; char label[24]; } door_action_t;
typedef struct {
    char name[48]; char user[40]; bool door; bool group; bool mobile;
    door_action_t actions[NET_MAX_DOOR_ACTIONS];
    int  n_actions;
} intercom_target_t;

// A room in the multiroom "add-rooms" list (driver reply to `getrooms`, PROTOCOL.md).
// `grouped` = shares this room's media session (checkbox state; our own room = true);
// `playing` = that room's queue is playing; `active` = it's in some session.
typedef struct { char id[24]; char name[48]; bool grouped, playing, active; } room_t;

// Security partition snapshot (driver-pushed `secstate`, PROTOCOL.md). Partitions
// are now auto-discovered per room via GET_SECURITY_DEVICES (driver.lua
// BuildSecurityList) instead of a manual Composer binding -- confirmed live
// 2026-09-14 that this room command is genuinely room-scoped for Security (same
// family as GET_LIGHT_DEVICES/GET_BLIND_DEVICES): room 2434 "Office" returned TWO
// real partitions (2684 "Home", 2685 "Office"), while every other tested room
// returned just 2684. `available` is false when the room resolves zero real
// partitions -- the UI hides the Security tile/page entirely in that case rather
// than showing a broken one. Everything in `list[]` is only meaningful when
// `available` is true.
//
// `list[].state` mirrors the Control4 partition proxy's PARTITION_STATE string
// verbatim (e.g. "DISARMED_READY", "ARMED_HOME", "EXIT_DELAY", "ALARM") rather
// than a firmware-defined enum, so a value this build doesn't recognize still
// displays (falls back to showing the raw string) instead of silently going
// blank.
//
// Per-zone open/closed/bypassed status is NOT modeled here: the driver-side
// SECURITY_PANEL proxy exposes no polled zone list, only a push notify whose
// exact payload shape is unconfirmed against a live Director (see driver.lua
// WatchSecurityVars and PROTOCOL.md) — `open_zones`/`last_faulted` are the only
// zone-shaped fields wired end-to-end today.
//
// Live-confirmed max is 2 partitions in one room (Office); this is a generous
// sanity ceiling against a hostile/buggy driver, same idea as NET_MAX_COMFORT.
#define NET_MAX_PARTITIONS 4
typedef struct {
    int  id;                   // Control4 device id of this partition — sent back
                                // verbatim in `secarm`/`secdisarm` to address it
    char title[48];             // partition name (e.g. "Home", "Office"), DeviceName(id)
    char state[24];            // PARTITION_STATE, e.g. "DISARMED_READY"
    char display[64];          // DISPLAY_TEXT
    char trouble[64];          // TROUBLE_TEXT
    int  open_zones;           // OPEN_ZONE_COUNT
    int  delay_total;          // DELAY_TIME_TOTAL (seconds)
    int  delay_remaining;      // DELAY_TIME_REMAINING (seconds), counts down during entry/exit delay
    char alarm_type[16];       // ALARM_TYPE
    char armed_type[16];       // ARMED_TYPE
    char last_faulted[48];     // LAST_ZONE_FAULTED, e.g. "Pool Bath Door"
} partition_t;

typedef struct {
    bool available;
    partition_t list[NET_MAX_PARTITIONS];
    int  n;
} security_state_t;

// Outcome of a `secarm`/`secdisarm` request (driver-pushed `secresult`). This is
// delivery confirmation ONLY — "the driver called C4:SendToProxy" — never proof
// the partition actually armed/disarmed. That proof, if it comes, is the next
// `secstate` showing the requested PARTITION_STATE for partition `id`; the UI
// must not report success from `ok` alone (fail-safe requirement, see ui.c).
typedef struct {
    int  id;            // which partition this result is for
    bool ok;
    char action[12];    // "arm" | "disarm"
    char error[48];     // set when ok == false, e.g. "no code entered"
} security_result_t;

// One thermostat's current state (driver-pushed `comfortlist`, PROTOCOL.md). `id` is
// the Control4 device id — sent back verbatim in a `comfortcmd` to control this exact
// thermostat. `temp`/`heat`/`cool` are already converted to whole display-unit
// degrees by the driver (confirmed live: the underlying Control4 variables are tenths
// of a degree CELSIUS regardless of what the thermostat's own SCALE variable claims to
// display — see driver.lua ReadComfortState — so the device never has to guess a
// conversion). `has_heat`/`has_cool` are false when the thermostat reports no
// setpoint at all (observed live for an "off"-mode unit) — `heat`/`cool` are
// meaningless in that case and must not be shown. `mode` is the thermostat's
// HVAC_MODE, lower-cased by the driver for a consistent wire vocabulary: "off" |
// "heat" | "cool" | "auto". `fan` is the raw FAN_MODE string (confirmed live LIST
// values: "on" | "auto"). `scale` is "F" or "C" — the unit suffix to display next to
// the numbers, not something the device converts by itself.
typedef struct {
    int  id;
    char title[64];
    bool has_temp;   // false: the thermostat reports no reading (wire omits `temp`)
    int  temp;
    bool has_heat; int heat;
    bool has_cool; int cool;
    char mode[8];
    char fan[8];
    char scale[2];
} comfort_t;

// Comfort page snapshot (driver-pushed `comfortlist`, PROTOCOL.md). `available` is
// false when the "Show Comfort" dealer property is Hidden — the UI hides the Comfort
// tile/page entirely in that case, same idea as `security_state_t.available`. Unlike
// Security, this is NOT gated on a bound proxy — GET_COMFORT_DEVICES is a fixed,
// confirmed-live, NOT-room-scoped house-wide thermostat list (same list regardless of
// which room asks, verified against 4 different rooms), so there is no per-instance
// binding to hide behind; the dealer property is the only gate.
typedef struct {
    bool available;
    comfort_t list[NET_MAX_COMFORT];
    int  n;
    // Optional (PROTOCOL.md `comfortlist`): which entry of the house-wide list is
    // THIS panel's room (from the room's own thermostat binding), and the outdoor
    // reading in that thermostat's scale. 0 / false when the driver sent neither.
    int  room_id;
    bool has_outdoor;
    int  outdoor;
} comfort_state_t;

// A `comfortcmd` request outcome would be nice, but unlike secarm/secdisarm there is
// no PIN/fail-safe requirement here (a thermostat setpoint bump is not a security
// action) — the confirming `comfortlist` push IS the acknowledgment, so no separate
// result message exists on the wire (see PROTOCOL.md "Comfort page").

typedef struct {
    int  id;
    char label[32];
    bool on;
    char color[7];   // "rrggbb"
    char icon[16];     // on-state icon (or always, if off_icon is empty)
    char off_icon[16]; // off-state icon ("" -> use icon)
} key_btn_t;

typedef struct {
    char room[48];
    bool power;
    bool playing;
    char media_type[12];          // driver sends "media" when metadata exists (PROTOCOL.md)
    char title[96], artist[96], album[96], art_url[300];
    id_name_t source;             // currently selected source
    int  volume;                  // 0..100
    bool muted;
    int  duration, position;      // seconds; 0 = unknown
    id_name_t meta[NET_MAX_META]; // track-info pairs (label in .id, value in .name)
    int       n_meta;
    key_btn_t buttons[NET_MAX_BUTTONS];
    int       n_buttons;
    // Dealer-configurable now-playing element visibility (driver-pushed; default shown).
    bool show_title, show_artist, show_info, show_progress;
    // Source transport capabilities (driver-pushed from the source's <Dashboard>).
    bool can_pause, can_stop, can_next, can_prev, can_thumbs_up, can_thumbs_down;
    bool can_shuffle, can_repeat;
    bool shuffle_on, repeat_on;   // real state (driver reads the digital-audio session)
} media_state_t;

// Callbacks run on the net server task. The app must keep them light / thread-safe
// (e.g. marshal UI work onto the LVGL task).
typedef struct {
    void (*on_connect)(void);
    void (*on_disconnect)(void);
    void (*on_state)(const media_state_t *st);
    void (*on_identify)(void);
    void (*on_display_change)(void);   // driver pushed orientation/layout (already saved)
    void (*on_announce)(const char *text, bool chime);  // Control4 announcement
    void (*on_endpoints)(const intercom_target_t *eps, int n);  // intercom picker targets
    void (*on_rooms)(const room_t *rooms, int n);               // multiroom add-rooms list
    void (*on_favorites)(const favorite_t *favs, int n);        // room navigator favorites
    void (*on_security)(const security_state_t *sec);           // security partition state
    void (*on_security_result)(const security_result_t *res);   // arm/disarm delivery outcome
    void (*on_comfort)(const comfort_state_t *cmf);              // Comfort page thermostat list
} net_callbacks_t;

// Start the TCP server (the DEVICE listens; the Control4 driver dials in).
void net_start(uint16_t port, const net_callbacks_t *cb);
bool net_connected(void);
// Drops the current driver link (if any) so the Director dials back in fresh --
// e.g. an on-screen "Reconnect to driver" button. Async: closes the socket out
// from under serve_client()'s blocking recv(), which does its own cleanup and
// fires on_disconnect; there is nothing else to wait on here.
void net_force_disconnect(void);
void net_get_ip(char *buf, size_t n);   // primary local IPv4 string ("" if offline)
const char *net_active_transport(void); // "Ethernet" / "Wi-Fi" / "" when nothing is addressed
int  net_driver_proto(void);            // driver's protocol version (0 = unknown/offline)
const char *net_peer_ip(void);          // connected Director/driver IP ("" if offline)
const char *net_current_room(void);     // last room the driver reported ("" if none yet)

// ── Transport-agnostic helpers (work for both WiFi-STA and Ethernet boards) ──
// The active network interface (Ethernet if present, else WiFi STA), or NULL.
struct esp_netif_obj;  // opaque; callers cast via <esp_netif.h>
struct esp_netif_obj *mmk_default_netif(void);
// Read the unit's base MAC for the active transport (ETH on wired boards, else WiFi).
void mmk_read_mac(uint8_t mac[6]);

// Write one newline-framed JSON line to the connected driver (no-op if none).
// Thread-safe; used by the intercom/SIP module (sip.c) to multiplex `sipstate`/
// `callstate` onto this same :6700 socket instead of a second TCP channel.
void net_send_line(const char *json);

// Commands -> driver (see PROTOCOL.md). Safe to call from any task.
void net_cmd(const char *c);          // play|pause|playpause|next|prev|stop
void net_set_volume(int level);       // 0..100
void net_step_volume(bool up);
void net_toggle_mute(void);
void net_art_debug(int slot, bool ok, int bytes, int w, int h);  // thumbnail telemetry

// Intercom call control from the on-screen buttons.
void net_call_mute(bool on);           // report mic mute up (-> NOTIFY.Mute_Audio_Changed)
void net_call_door(const char *remote, const char *action_id);   // fire a door action
void net_select_source(const char *id);
void net_send_button(int id);         // programmable keypad button tap
void net_request_rooms(void);         // ask the driver for the multiroom list (-> on_rooms)
void net_group_room(const char *id, bool join);   // multiroom join/leave a room (add-rooms tap)
void net_request_favorites(void);                 // ask for this room's favorites (-> on_favorites)
void net_play_favorite(const char *id);           // play the favorite tile with this id
void net_ping(void);

// Security partition arm/disarm (see PROTOCOL.md `secarm`/`secdisarm`). `partition_id`
// is the partition_t.id to address — a room can auto-discover more than one (see
// security_state_t above), so every request now names its target explicitly instead
// of assuming a single bound partition. `pin` rides this one call — never persisted,
// logged, or echoed by net.c or the UI; the driver forwards it straight to Control4's
// UserCode and drops it. `arm_type` is one of "Stay" | "Away" | "Stay Instant" |
// "Away Instant" (the partition proxy's own vocabulary — passed through verbatim).
// `bypass` asks the driver to arm with any currently-faulted zones bypassed rather
// than refusing to arm.
void net_security_arm(int partition_id, const char *arm_type, const char *pin, bool bypass);
void net_security_disarm(int partition_id, const char *pin);

// Comfort page thermostat control (see PROTOCOL.md `comfortcmd`). `id` is the
// comfort_t.id of the thermostat to control. `action` is one of "heat_inc" |
// "heat_dec" | "cool_inc" | "cool_dec" | "mode" (driver.lua COMFORT_CMD /
// COMFORT_MODE_WIRE); `mode` is only meaningful (and only sent) for action "mode",
// one of "off" | "heat" | "cool" | "auto". There is no PIN and no fail-safe result
// message the way secarm/secdisarm has — a thermostat bump is not a security action,
// and the confirming `comfortlist` push (via the driver's variable-listener path) IS
// the acknowledgment.
void net_comfort_cmd(int id, const char *action, const char *mode);

// Report the device's current halo settings (g_settings) up to the driver, so
// Composer's Halo properties can mirror whatever is actually on the LED --
// the device is authoritative, so this is what keeps them from drifting after
// a local edit, a fresh device that's never talked to Composer, or the driver
// having missed a change while offline. Call after ANY change to the halo
// fields in g_settings, from whichever side made it. No-op if not connected
// or the board has no halo hardware.
void net_report_halo(void);

// One-off diagnostic line relayed to the driver (-> director.log via dbg(),
// gated on that device's Debug Logging like "sent ota"/"pushed halo" already
// are). For events with no other visibility into a panel with no physical/USB
// access -- see ui.c's wake-shield/backlight tracing. No-op if not connected.
void net_send_diag(const char *msg);
