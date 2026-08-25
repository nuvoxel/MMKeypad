# Control4 media command vocabulary — verified

> Verified 2026-07-22 by live-Director RE (OS 4.2.1, EA-3). Sources: first-party
> **unencrypted** `room_control_keypad.c4z` driver.lua (v139), the official MSP SDK
> sample (`snap-one/docs-driverworks`), and live spikes against the Office session
> (room 2434, source "Music A" = `media_service` dev 2687). Design rationale lives in
> [../../docs/DATA-PLANE.md](../../docs/DATA-PLANE.md); this file is the API cheat-sheet.

## The one rule that organizes everything

**CONTROL goes through the ROOM (open to any driver). BROWSE needs a NAVIGATOR (gated).**

- Room commands — `C4:SendToDevice(roomId, …)` — select/group/transport/enumerate. We
  already address the room via `C4:RoomGetId()`. No special standing required.
- MSP browse — executing catalog commands on a service driver — requires navigator
  identity we don't yet have. Only album/artist/search browsing needs this.

## Room / device IDs (dev system, for reference)

| Thing | ID |
|---|---|
| Office room | 2434 |
| Digital Media agent (`C4_DIGITAL_AUDIO`) | 100002 |
| "Music A" media_service (streaming source) | 2687 |
| Triad SA1 controller | 3110 (audio dev 3109) |
| SA1 `UIMediaInfo` child (UI_MEDIAINFO) | 3111 |
| Office Neeo Remote (a driver acting as navigator) | 3391 |

## Source selection (verified — room_control_keypad)

**Enumerate the room's sources:**
```lua
C4:SendToDevice(roomId, 'GET_WATCH_DEVICES',  {})            -- video sources
C4:SendToDevice(roomId, 'GET_LISTEN_DEVICES', {})            -- audio sources
C4:SendToDevice(roomId, 'GET_WATCH_DEVICES',  {hidden = 1})  -- + hidden
-- returns XML with <device_id>…</device_id> entries; parse for the list
```

**Select a source into the room — BRANCH on source type:**
```lua
local isDigital = (string.lower(C4:GetDeviceData(src,'digital_audio_support') or '') == 'true')
if isDigital then
  -- STREAMING sources (media_service: Music A/SiriusXM/TuneIn/Pandora/AirPlay).
  -- Sent to the SOURCE, not the room. Streaming sources IGNORE SELECT_AUDIO_DEVICE.
  C4:SendToDevice(src, 'DEVICE_SELECTED', {idRoom = roomId})
else
  -- Regular AV source, sent to the room.
  C4:SendToDevice(roomId, 'SELECT_AUDIO_DEVICE', {deviceid = src})   -- audio
  C4:SendToDevice(roomId, 'SELECT_VIDEO_DEVICE', {deviceid = src})   -- video
end
```
> ⚠️ The streaming-vs-AV branch is the landmine the old "unverified param tables" note
> was hiding. Get it wrong and streaming selection silently no-ops.

## Favorites — TWO kinds, different play stories (verified 2026-07-22)

### Kind A — Broadcast-Audio media (room command, ungated)
```lua
-- Play a station/preset from a source's Broadcast Audio section of the C4 Media DB.
-- mediaid = numeric id from the "Preset Source Media" list. Not all sources populate it.
C4:SendToDevice(roomId, 'SELECT_AUDIO_MEDIA', {
  deselect = '0', type = 'BROADCAST_AUDIO', mediaid = mediaId,
})
```

### Kind B — Navigator favorite tiles (read ungated; play gated for streaming)
The user-facing favorite tiles (cross-source). Managed by the **UI Configuration agent
(dev 1609)**.
```lua
-- READ — works from any driver, no navigator identity, returns synchronously:
local xml = C4:SendUIRequest(1609, 'GET_ALL_ROOM_FAVORITES_STATE', {})
-- <rooms><room><room_id>N</room_id><favorites_state>
--   <favorite><id>..</id><path>..</path><title>..</title><image>..artworkURL..</image>
--            <menu>listen|comfort|security|..</menu><type>..</type><tileSize>..</tileSize></favorite>
-- Media favorite path = /v1/rooms/{room}/mediaservicefavorite/{sourceDev}?context=..{catalogRef}..
-- Other agent verbs: GET_FAVORITE, GET_FAVORITE_LIST, SET_FAVORITE, DELETE_FAVORITE,
--                    SET_FAVORITE_ORDER, GET_PRESETS, GET_ALL_ROOM_STATE. (No EXECUTE/PLAY verb.)
```
- **PLAY of a media-service favorite is GATED** — its `path` is a navigator/cerebellum
  endpoint (POST to director :443 → 404). The catalog ref (e.g. `ra.985488606`) is NOT a
  broadcast-audio mediaid, so `SELECT_AUDIO_MEDIA` can't play it. No room-command path.
- **Fallback (ungated):** tapping a streaming favorite → select its source
  (`DEVICE_SELECTED {idRoom}` to the source dev). Resumes last content, not the exact
  station. Exact station/playlist play only via Kind A (broadcast-audio) favorites.
- The old note "FAVORITE_0..N readable room vars / SET_NAVIGATOR_ROOM_FAVORITES" was
  wrong: `GetDeviceVariable` can't read them by name; use the agent-1609 read above.

## Multiroom grouping (join/leave a session)

```lua
-- Add rooms to the session owned by ownerRoomId. ROOM_ID_LIST is comma-joined room ids.
C4:SendToDevice(100002 --[[C4_DIGITAL_AUDIO]], 'ADD_ROOMS_TO_SESSION', {
  ROOM_ID = ownerRoomId, ROOM_ID_LIST = 'r2,r3,…',
})
```
This is the WRITE side of the rooms list (the firmware add-rooms panel, stubbed since
2026-07-21). Read side = session→rooms membership from var 1006 (`QUEUE_STATUS_V2`).

## Transport / room

```lua
C4:SendToDevice(deviceId, 'PLAYPAUSE', {})
C4:SendToDevice(roomId,   'ROOM_OFF', {})
C4:SendToDevice(roomId,   'SET_VOLUME_LEVEL', {LEVEL = n})   -- n = 0..100
```

## Now-playing data feeds (native, NOT tr2)

- **MediaSession events** — `OnMediaSessionMediaInfoChanged` (Stage 1). C4's own push,
  structured, keyed by session. Primary feed.
- **`UI_MEDIAINFO` proxy — SPIKED 2026-07-22, NOT a viable tap.** Provider declared by
  audio endpoints (`control4_sa1` etc.); the SA1's `uimediainfo.c4i` child (dev 3111) is
  the consumer. A throwaway Lua consumer driver (declared a `UI_MEDIAINFO` consumer
  connection) **loaded fine and could self-`C4:Bind` to the SA1 provider (3109:5002) with
  `ok=true`** — BUT `GetBindings` showed it never actually attached (`3402:5001` had no
  boundprovider; `3109:5002` stayed bound only to `3111`). **UI_MEDIAINFO is a 1:1
  provider→consumer binding, already claimed by the source's own child.** A second
  consumer cannot passively tap it; you'd have to displace `3111` (take over the room's
  media-info UI). ⚠️ `C4:Bind` returning `ok=true` does NOT mean it bound — verify with
  `GetBindings`. **Conclusion: use MediaSession events (broadcast, Stage 1) for
  now-playing, not UI_MEDIAINFO.**

## Deep catalog browse — GATED (MSP navigator protocol)

Only album/artist/search browsing. The navigator **executes a command on the service
driver** (not `SendUIRequest`, which returns nil):

```
command: Browse | GetDashboard | GetQueue | SelectItem | ToggleShuffle | ToggleRepeat | PLAY/PAUSE/…
params:  NAVID (opaque per-navigator id), ROOMID (int), SEQ (int), CMD, LOCALE, ARGS (XML)
reply:   async to the proxy's UI listeners:
         <RESPONSE><NAVID/><SEQ/><DATA/></RESPONSE>  + <EVENT> pushes
         (ProgressChanged / QueueChanged / DashboardChanged)
```

**Why we can't just do this yet:**
- `SendToDevice(2687,'GetDashboard',…)` from our unbound keypad returns `ok=true` but is
  **silently dropped** — browse requires navigator standing.
- NAVID is **engine-minted** (native `uidevice.c4l` + `libmediamanager.so`, which own the
  `c4:navigator` scheme). Neeo's NAVID (`e003…`) is issued per navigator instance;
  making one up is moot while the command can't route.
- **Proof a driver CAN be a navigator:** live log showed
  `Driver [id: 3391 "Office Neeo Remote"] executing command "GetQueue" on driver [2687 "Music A"]`.
- **Open question:** how a non-navigator driver gets navigator standing — read
  `libmediamanager.so`, or find the room-mediated send Neeo uses (it goes *through the
  room*, which is why our direct-to-device send drops).

## Dead ends (do not re-investigate)

- **`nowplaying-hdmiout.c4z`** (SA1 "Now Playing", dev 3112) — puts cover-art OSD on a
  **TV** via HDMI (`SET_WATCH_DEVICE_ORDER`, `SET_VIDEO_SELECTION_ONLY`). Pixels on a
  display; useless for a self-rendering keypad.
- **List Navigator** (`ListNewList`/`ListMIBReceived`) — `room_control_keypad` does NOT
  use it for media; media is its preset-source room commands. Legacy, not our route.
- **tr2/tr3 cerebellum** (`/opt/control4/tr3/cerebellum/cerebellum.js`) — graphical
  browse renderer for touchscreens. We want data, not pixels.

## Dev harness

`scratchpad/nvlua.sh <deviceId> '<lua>'` fires arbitrary Lua in a live keypad instance
(via the `LUA_COMMAND` handler in driver-keypad/driver.lua) and reads back tagged
`ErrorLog` output. The handler is **DEV-ONLY, gated on Debug Logging ≠ Off, and MUST be
stripped before publish** (remote-eval backdoor; flagged in-code). See
our internal Director-access notes (not published) for Director access.
