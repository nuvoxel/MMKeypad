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

typedef struct { char id[24]; char name[48]; } id_name_t;

// A room navigator favorite tile (driver reply to `getfavorites`, PROTOCOL.md).
// Room-level (no source id). `id` is the opaque favorite id sent back in a `favorite`
// message to play it; `title` is the display label; `art_url` is the tile artwork URL
// (may be empty); `kind` is `stream` | `broadcast` (informational — the driver enacts
// the play; the device just sends the id).
typedef struct { char id[40]; char title[96]; char art_url[300]; char kind[12]; } favorite_t;

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
} net_callbacks_t;

// Start the TCP server (the DEVICE listens; the Control4 driver dials in).
void net_start(uint16_t port, const net_callbacks_t *cb);
bool net_connected(void);
void net_get_ip(char *buf, size_t n);   // primary local IPv4 string ("" if offline)
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
