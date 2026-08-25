# Keypad Data Plane & Scaling — Design Direction

> Status: **Stages 1–3 shipped; Stage 4 write-path unblocked (only deep browse gated).**
> Born from the 2026-07-21 Director CPU incident and the RE that followed; media
> data/control architecture resolved 2026-07-22. Companion to
> `ARCHITECTURE.md` and `PROTOCOL.md` — which now live in the open repo,
> [github.com/nuvoxel/MMKeypad](https://github.com/nuvoxel/MMKeypad) — and
> our internal Director-access notes (not published). RE assets in [reference/](reference/), verified
> command vocabulary in [../reference/control4/media-commands.md](../reference/control4/media-commands.md).

## Why this doc — the lesson from the CPU incident

Streaming audio made the dev Director "basically unusable." Root cause was an
architecture that scaled as `O(keypads × event-rate)` on the Director's **single
Lua thread**: every keypad instance subscribed **globally** to MediaSession events
and re-derived **full** room state (var scrapes + `GetDeviceVariables` dumps + XML
re-parse) on every trigger.

## Proven facts (measured, not assumed)

1. **All driver Lua runs on ONE shared thread.** *Proven* by a controlled CPU-burn
   in the **agent** driver: it pegged TID 31625 — the *same* thread the keypad
   spin pegged — and total director CPU capped at ~98% of one core under two
   streams. **Consequence: you cannot offload to another core inside DriverWorks.**
   Moving work between our drivers (keypad↔agent) reduces *duplication* but never
   buys parallelism. True core-offload needs an off-box process — unavailable on a
   customer Director (no root; we are only a driver/agent).

2. **The media session is the authoritative now-playing source.**
   `OnMediaSessionMediaInfoChanged` delivers structured `title/artist/album/<img>/
   mediaType/mediaTypeV2/stationid/streamStatus` — no room-var scraping needed. It
   is C4's own C++ push (better than any proxy binding; there is no consumable
   media proxy). Keyed by **session**, delivered **globally** to every instance.

3. **Var 1006 (`QUEUE_STATUS_V2`) carries session→room membership + queue state.**
   Captured live:
   ```xml
   <queues><queue><id>10006</id><owner>2434</owner>
     <shuffle>0</shuffle><repeat>0</repeat><state>Play</state>
     <device_id>2687</device_id><rooms><id>2434</id></rooms></queue></queues>
   ```
   One var read gives **owner room, member rooms, shuffle, repeat, play-state**.

4. **Caps / isRadio / catalog-browse are NOT device vars.**
   `GetDeviceVariables(sourceDevice)` came back empty. Deep-catalog data lives in the
   **MSP (Media Service Proxy) browse layer** — the same feed the X4 Navigator uses.
   ⚠️ **Correction (see "Media data & control" below):** it is reached via the **MSP
   navigator protocol** (a command executed *on the service driver* with NAVID/SEQ),
   **NOT** `SendUIRequest` (which returns nil). And favorites/source-select/grouping do
   NOT need it at all — those are room commands. Only album/artist/search *browsing* is
   gated on navigator identity.

## The "real source" table

| Data | Authoritative source | Mechanism |
| --- | --- | --- |
| Now-playing (title/artist/album/art) | MediaSession events | `OnMediaSessionMediaInfoChanged` payload |
| Content type (isRadio hint) | MediaSession events | `mediaType` / `mediaTypeV2` / `queueInfo` |
| Session → member rooms (rooms list) | var 1006 | `<rooms>` / `<owner>` |
| Shuffle / repeat / play-state | var 1006 | `<shuffle>/<repeat>/<state>` |
| Source list (room) | room command | `GET_WATCH_DEVICES` / `GET_LISTEN_DEVICES` on roomId |
| Favorites / stations (play) | room command | `SELECT_AUDIO_MEDIA` (broadcast-audio) |
| Source select | room command | `SELECT_AUDIO_DEVICE` / `DEVICE_SELECTED` (streaming) |
| Multiroom grouping | Digital Media agent (100002) | `ADD_ROOMS_TO_SESSION` |
| Deep catalog browse (albums/artist/search) | **MSP navigator** | command-on-service w/ NAVID (gated — see below) |
| Volume / mute | MediaSession events + room vars | `OnMediaSessionVolumeLevelChanged` |

## Hard constraints (production reality)

1. **No root / no on-box process** on a customer Director. We are only a
   DriverWorks Lua driver + agent, sharing the one Lua thread. On-box Node / cerebellum
   taps are **dev-only**.
2. **We own the keypad firmware** (neutral TCP server; a client dials in and pushes
   state — stays ecosystem-agnostic; do NOT make the firmware Control4-aware).
3. **The driver.xml surface is FROZEN** — proxies (keypad 5002, intercom 5003),
   connections, commands, events, variables. Changing them corrupts existing
   installs. Rewrites are **in-place Lua only** (+ additive properties/list-items).
4. **Goals:** performant · good neighbor · scale to 20 keypads · stay on the right
   side of the line (read-mostly gray usage; not a Navigator clone; isolate any gray
   dependency behind a swappable adapter).

## Architecture

Keep the firmware neutral. Reshape the Control4 client only:

- **Data plane, event-sourced + early-out (Stage 1, shipped).** Each keypad
  instance caches its room's session `<deviceid>` (var 1031) and **drops media
  events for other rooms' sessions before any work**. Only the owning instance
  builds/pushes. Cost scales with *active sessions*, not keypad count.
- **Coordinator (agent), additive.** House-wide session→room map (from var 1006) →
  pushes the **rooms list** + **source/favorite/recent lists** to keypads. The
  keypad stays self-sufficient for its own room (no hard dependency on the agent).
- **Use the C-side sources:** MediaSession events + var 1006 for state; **room commands**
  (`SendToDevice(roomId,…)`) for source-select / favorites / grouping / transport; the
  **MSP navigator protocol** only for deep catalog browse (gated — see "Media data &
  control"); `intercomproxy` (calls) and `keypad_proxy` (buttons) unchanged.

Rejected: **keypad-reads-Control4-directly** (breaks firmware neutrality, loses the
driver's free in-process access, needs a gray auth path). **Binding a media proxy**
(no consumable media proxy exists; the events already deliver the data).

## Measured results (Stage 1)

| Build | director CPU (streaming) |
| --- | --- |
| Original | ~100%+ (unusable) |
| + coalesce triggers + cache static source data | ~40% |
| + session early-out + `QueueState` single-var read | **~14–18%** |

Stage 1 is in-place, reload-safe, no surface change, and scales with active
sessions rather than keypad count.

## Roadmap

- **Stage 1 — keypad data plane. ✅ shipped.** Coalesce (`SchedulePush`), cache
  static source caps/icon, session early-out (`gRoomSessionDev`), `QueueState` reads
  var 1006 directly.
- **Stage 2 — good-neighbor.** Intercom driver's 2s always-on poll → event-driven /
  idle backoff. Fix the streaming-update lag (150ms debounce never fires under
  continuous events). Move shuffle/repeat onto the var-1006 read we already do.
- **Stage 3 — coordinator (agent).** Maintain session→room from var 1006; push a
  `rooms` list to keypads (additive, forward-compatible). Enumerate sources via
  `GET_WATCH/LISTEN_DEVICES`. **Write side (grouping) is unblocked** — see below.
- **Stage 4 — write path + firmware un-stub. Unblocked (2026-07-22 RE).** Scope decision
  (2026-07-22): **deep catalog browse is DROPPED** (albums/artist/search — the only thing
  that needed MSP navigator identity). Content access is via **favorites + source select**
  instead, all verified room commands, no navigator identity. Build: **select-source**
  (`SELECT_AUDIO_DEVICE` / `DEVICE_SELECTED` for streaming), **play-favorite**
  (`SELECT_AUDIO_MEDIA` broadcast-audio), **join/leave room** (`ADD_ROOMS_TO_SESSION`),
  transport/volume, then the firmware un-stub (rooms list, source picker, favorites,
  transport). Full vocabulary + copy-from reference: "Media data & control" and
  [../reference/control4/media-commands.md](../reference/control4/media-commands.md).

  **Favorites — two flavors, know which you're shipping:**
  1. **Broadcast-Audio presets** (`SELECT_AUDIO_MEDIA {type='BROADCAST_AUDIO', mediaid}`)
     — ✅ fully verified (room_control_keypad). Covers station/preset favorites for sources
     that populate the C4 Media DB's Broadcast Audio section (TuneIn, SiriusXM, etc.).
     No gate. **This is the default favorites path.**
  2. **Navigator favorite tiles** (the cross-source tiles users configure) — **SPIKED
     2026-07-22, split result:**
     - **READ is unblocked.** `C4:SendUIRequest(1609, 'GET_ALL_ROOM_FAVORITES_STATE', {})`
       (dev 1609 = UI Configuration agent) returns ALL rooms' favorites synchronously, no
       navigator identity. Each favorite: `id`, `path`, `title`, `image` (artwork URL),
       `menu`/`type`, `tileSize`. Media favorites carry
       `path=/v1/rooms/{room}/mediaservicefavorite/{sourceDev}?context=…{catalogRef}…`
       — i.e. source device + a URL-encoded catalog item ref (e.g. Apple Music station
       `ra.985488606`). So we can fully *display* favorites incl. streaming stations.
     - **PLAY is GATED for streaming favorites.** The favorite's `path` is a
       **navigator/cerebellum** endpoint (POST to director :443 REST → 404). Executing a
       media-service favorite means driving cerebellum as a navigator — the layer we
       dropped. The catalog ref (`ra.985488606`) is NOT a broadcast-audio `mediaid`, so it
       can't go through `SELECT_AUDIO_MEDIA`. No room-command play path exists for these.
     - **Net:** the user's favorites are Apple Music stations (streaming) → exact-play is
       navigator-gated. Display navigator favorites (read works); tapping a streaming
       favorite falls back to selecting its source; broadcast-audio favorites play exactly.
     - **BUILT (2026-07-22, path 2).** Driver: `BuildFavoritesList()` reads agent 1609
       (`GET_ALL_ROOM_FAVORITES_STATE`), filters to media tiles, caches by favId in
       `gFavorites` {kind, src|mediaid, title}; `DoPlayFavorite(msg)` resolves by id —
       `broadcast`→`SELECT_AUDIO_MEDIA`, `stream`→`SelectSourceInRoom` (source fallback).
       Protocol: `getfavorites` (room-level) / `favorites` [{id,title,image,kind}] /
       `favorite` {id}. Verified live: Office returns the 2 Apple Music station tiles with
       artwork. Firmware favorites grid UI: in progress.

## Feature notes

- **Multiroom / rooms list** — the X4 "add-rooms" control (`ui.c`, stubbed to self).
  Read side = session→rooms from var 1006 (Stage 3, cheap). Write side = a room
  joining/leaving a session (Stage 4 spike). The agent already enumerates rooms
  (`projectRooms()`).
- **Source selection** — ✅ **verified (2026-07-22).** List = `GET_WATCH_DEVICES` /
  `GET_LISTEN_DEVICES` on the roomId; select = `SELECT_AUDIO_DEVICE {deviceid}` to the
  room for AV, or `DEVICE_SELECTED {idRoom}` to the SOURCE for streaming (branch on
  `digital_audio_support`). See [commands cheat-sheet](../reference/control4/media-commands.md).
- **Favorites** — two forms: room-var favorites `FAVORITE_0…N` (driver-readable;
  `SET_NAVIGATOR_ROOM_FAVORITES` to write, native-engine path) **and** broadcast-audio
  station play via `SELECT_AUDIO_MEDIA {type='BROADCAST_AUDIO', mediaid}` (the
  "Preset Source Media" list, populated by the source's Broadcast Audio DB section).
- **Recents / deep catalog** — the one feature still gated on MSP navigator identity.
- **Transport buttons / isRadio** — `GetSourceCaps` (static Dashboard) is the wrong
  source; authoritative caps come from the media layer for the current content.
  `mediaTypeV2` is a usable interim signal.

## Media data & control — the resolved architecture (2026-07-22 RE)

A day of live-Director RE (spikes against the Office session: source "Music A" =
`media_service` dev 2687 playing Apple Music, room 2434) settled how the keypad should
get media data and drive sources. The verified command vocabulary is in
[../reference/control4/media-commands.md](../reference/control4/media-commands.md); this is
the architecture and the *why*.

**The key split: CONTROL goes through the ROOM; BROWSE needs a NAVIGATOR.**
`C4:SendToDevice(roomId, …)` room commands are open to any driver (we already address
the room via `C4:RoomGetId()`). MSP *browse* (album/artist/search lists) is gated on a
navigator identity we don't have. This split is what everything below reduces to, and it
means **all of Stage 3–4 except deep browse is buildable now, with no gray dependency.**

### What each feature rides on

| Feature | Mechanism | Who can do it |
|---|---|---|
| Now-playing display | MediaSession events (shipped) or `UI_MEDIAINFO` consumer proxy | any driver |
| Session→rooms, shuffle/repeat/state | var 1006 (`QUEUE_STATUS_V2`) | any driver |
| Source select (AV) | `SendToDevice(roomId,'SELECT_AUDIO_DEVICE',{deviceid})` | any driver |
| Source select (streaming) | `SendToDevice(src,'DEVICE_SELECTED',{idRoom})` — **note: to the SOURCE** | any driver |
| Favorites/stations play | `SendToDevice(roomId,'SELECT_AUDIO_MEDIA',{type='BROADCAST_AUDIO',mediaid})` | any driver |
| Multiroom grouping | `SendToDevice(100002,'ADD_ROOMS_TO_SESSION',{ROOM_ID,ROOM_ID_LIST})` | any driver |
| Transport / volume / off | room commands (`PLAYPAUSE`, `SET_VOLUME_LEVEL`, `ROOM_OFF`) | any driver |
| Enumerate room sources | `SendToDevice(roomId,'GET_WATCH_DEVICES'/'GET_LISTEN_DEVICES',{})` | any driver |
| **Deep browse** (albums/artist/search) | **MSP navigator commands** (`Browse`/`GetDashboard`/`GetQueue`) | **navigator only — OPEN** |

### Now-playing: two sanctioned data feeds, neither is tr2

Confirmed the SA1 (and audio endpoints generally) surface now-playing through **native
C++**, not tr2/tr3/cerebellum:
- **MediaSession events** (`OnMediaSessionMediaInfoChanged`) — what Stage 1 uses; C4's
  own push, structured, keyed by session. Still the primary feed.
- **`UI_MEDIAINFO` proxy — SPIKED 2026-07-22, ruled OUT.** The SA1's `uimediainfo.c4i`
  child (dev 3111) consumes the provider (dev 3109 binding 5002). A throwaway Lua consumer
  driver loaded fine and self-`C4:Bind`'d with `ok=true`, but `GetBindings` proved it never
  attached: **UI_MEDIAINFO is a 1:1 provider→consumer binding, already claimed by the
  source's own child.** Can't passively tap it without displacing 3111. So now-playing
  stays on **MediaSession events** (broadcast to all instances — the right bus). Detail +
  the `C4:Bind`-lies gotcha in [../reference/control4/media-commands.md](../reference/control4/media-commands.md).

tr2/tr3 lives ONLY in `/opt/control4/tr3/cerebellum/cerebellum.js` (obfuscated Node),
which renders the rich *graphical browse* for full touchscreens. We render our own UI, so
we never want its pixels — only the underlying data. **Not a path we pursue.**

### The write path (source/favorites/grouping) — verified, buildable now

Reference: `room_control_keypad.c4z` ships **unencrypted** and its "Preset Source 1-5"
feature is a complete working example (driver.lua, OS 4.2.1 v139, on the Director at
`/mnt/internal/c4z/room_control_keypad/`). Key correctness details baked into the
[commands cheat-sheet](../reference/control4/media-commands.md):
- **Streaming sources take a different command than AV sources** — `DEVICE_SELECTED` to
  the source vs `SELECT_AUDIO_DEVICE` to the room. Branch on
  `GetDeviceData(src,'digital_audio_support')`. Streaming sources (the MSP services)
  *ignore* `SELECT_AUDIO_DEVICE` — this was the landmine the unverified param tables hid.
- **Grouping** = `ADD_ROOMS_TO_SESSION` to the Digital Media agent (dev 100002). This is
  the write side of the rooms list the firmware panel was stubbed waiting for (Stage 3/4).

### Deep browse — the one thing still gated (MSP navigator identity)

Browsing a service's catalog (albums, artists, search, source-specific favorites) uses
the **MSP navigator protocol**: the navigator *executes a command on the service driver*
(`Browse`/`GetDashboard`/`GetQueue`/`SelectItem`), params `NAVID`/`ROOMID`/`SEQ`/`CMD`/
`LOCALE`/`ARGS`, and the service replies **asynchronously** to the proxy's UI listeners as
`<RESPONSE><NAVID/><SEQ/><DATA/></RESPONSE>` + `<EVENT>` pushes. Verified live: the
**Neeo remote (a driver, dev 3391) does exactly this** to Music A(2687) — proof a driver can
be a navigator.

But two things block us copying it directly, and one dead end was ruled out:
- `SendUIRequest(mspDevice,"GetDashboard",{})` returns **nil** — wrong interface.
- `SendToDevice(2687,"GetDashboard",…)` from our (unbound) keypad returns `ok=true` but is
  **silently dropped** — never routes to the service. Browse requires navigator standing.
- The NAVID is **engine-minted, not free-form Lua.** The navigator surface lives in
  native `uidevice.c4l` + `libmediamanager.so` (owns the `c4:navigator` URI scheme,
  `SET_NAVIGATOR_ROOM_FAVORITES`, `getActiveMediaInfo`, `MEDIA_PLAYER_GUI`). Neeo's NAVID
  (`e003…` hash) is issued by that engine per navigator instance. Making one up is
  pointless while the command can't route anyway. **We did NOT add a consumer connection
  or mutate the shipping driver on the earlier (wrong) "just add a MediaService
  connection / synthesize a NAVID" premise.**

**Open question (deep browse only):** how a non-navigator driver legitimately obtains
navigator standing — read `libmediamanager.so`, or find the room-mediated send navigators
actually use (Neeo goes *through the room*, which is why our direct-to-device send drops).
Everything else ships without answering this.

### Dead ends ruled out (don't re-investigate)

- **`nowplaying-hdmiout.c4z`** (SA1 "Now Playing", dev 3112) — a video-path source that
  puts the cover-art OSD on a **TV** via HDMI (`SET_WATCH_DEVICE_ORDER`,
  `SET_VIDEO_SELECTION_ONLY`). Produces pixels on a display; useless for a self-rendering
  keypad.
- **List Navigator** (`ListNewList`/`ListMIBReceived` callbacks) — the cited reference
  `room_control_keypad` does NOT use it for media (it's the *room* driver; media lives in
  its preset-source room commands). Legacy path, not our route.
- **tr2/tr3 cerebellum client** — graphical renderer only; we want data, not pixels.

## Dev tooling (this session)

- **`scratchpad/nvlua.sh <deviceId> '<lua>'`** — fires arbitrary Lua in a live keypad via
  the `LUA_COMMAND` handler and reads back tagged `ErrorLog` output. The spike workhorse.
- **`LUA_COMMAND` handler in driver-keypad/driver.lua** — remote-eval backdoor, **DEV-ONLY,
  gated on Debug Logging ≠ Off, MUST be stripped before publish** (flagged in-code).
- Director access + the SendToDevice/REST plumbing: our internal Director-access notes (not published).
