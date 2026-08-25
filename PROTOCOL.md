# MMKeypad Wire Protocol

The Control4 driver and the device firmware talk over a single **TCP socket**.
The **device is the server** (listens on port **6700**); the **driver connects
out** to it via a Control4 network binding (`C4:NetConnect`). This matches the
native model: each device is an independent IP device, bound on the Connections
page, so the driver dials its address — one driver instance per device, no shared
server-port collisions.

Framing: **one JSON object per line**, UTF-8, terminated by `\n` (LF). Both sides
buffer inbound bytes and split on `\n` (`ReceivedFromNetwork` may deliver
fragments).

Every message has a `t` (type) field. Unknown types MUST be ignored (forward-compat).

---

## Connection lifecycle

1. Device listens on TCP **6700**. The driver (once its binding is addressed)
   connects out to the device's IP:6700 and Director reports `ONLINE`.
2. Device sends `hello` (its MAC + fw) on accept.
3. Driver pushes an initial `state` for the **room the driver instance is placed
   in** (no room picker — the room is the driver's Control4 placement).
4. Driver pushes `state` on every relevant room/media change (debounced/deduped).
5. Either side may send `ping`; peer replies `pong`.

If the link drops, Director re-dials the binding (keep_connection/auto_connect);
the device just keeps listening.

---

## Device → Driver

| `t`      | Fields                                  | Meaning |
|----------|-----------------------------------------|---------|
| `hello`  | `mac` (str), `fw` (str), `intercom` (bool), `manifest` (obj) | Announce self (sent on connect). `intercom` = device has the SIP intercom; the driver only offers "Install Intercom" when true. `manifest` = the device self-description (see below) |
| `cmd`    | `cmd` ∈ `play\|pause\|playpause\|next\|prev\|stop\|thumbsup\|thumbsdown\|shuffle\|repeat\|roomoff` | Transport / rating / shuffle+repeat toggle / `roomoff` = turn the room off |
| `vol`    | `level` (0–100) **or** `dir` ∈ `up\|down` | Set absolute volume or step |
| `mute`   | —                                       | Toggle mute |
| `source` | `id` (str, device id to select)         | Switch the room's source. The driver picks the right room command per source type: streaming/digital-audio sources are selected on the SOURCE (`DEVICE_SELECTED`), AV sources on the room (`SELECT_AUDIO_DEVICE`/`SELECT_VIDEO_DEVICE`) — the device just names the id. |
| `getrooms` | —                                     | Request the multiroom list (device sends when the add-rooms panel opens) → driver replies with `rooms` |
| `grouproom` | `id` (str, room id), `join` (bool)  | Join (`true`) or leave (`false`) that room's multiroom grouping with this room (the add-rooms panel tap). Join = `ADD_ROOMS_TO_SESSION`; leave = the room is turned off (`ROOM_OFF`). |
| `getfavorites` | —                                | Request this room's favorite tiles (device sends when the favorites grid opens) → driver replies with `favorites`. Room-level (no source id). |
| `favorite` | `id` (str, favorite id)             | Play the favorite tile with this id. The driver resolves the play from the favorite's kind: `broadcast` → exact `SELECT_AUDIO_MEDIA`; `stream` → select the favorite's source (`DEVICE_SELECTED`, resumes that source — exact station play is navigator-gated). Legacy `mediaid` (str) instead of `id` still plays a broadcast-audio preset directly. |
| `button` | `id` (int, programmable keypad button)  | A keypad button was tapped → driver raises the keypad-proxy action |
| `ota`    | `status` ∈ `checking\|updating\|uptodate\|ok\|error`, `msg` (str, optional), `pct` (0–100, optional) | Firmware-update progress/result → driver's `Firmware Update Status` |
| `license`| `status` ∈ `ok\|error`                  | Result of a driver-relayed license push → driver's `License Status` |
| `ping`   | —                                       | Keepalive |

`hello` also carries the device **identity** — `hwid` (hex), `secret` (hex HMAC),
`sku` — so the driver (which has WAN via the Director) can fetch this keypad's
license/OTA from nuvoxel and relay it, for keypads with no internet of their own.

### Device `manifest` (self-description)

The `manifest` object in `hello` lets the device describe **what it is and what
it can do**, so the driver adapts to the device rather than hardcoding per-SKU
assumptions. Family-aware: the model is resolved from SoC + panel, so one build
serves several models (e.g. Control4 T3-7 / T3-10 / T4 / T5). Fields:

| Field | Example | Meaning |
|-------|---------|---------|
| `sku` | `"mmk-t3"` | platform SKU |
| `model` | `"Control4 T3-7"` | friendly hardware model |
| `board` | `"t3-7"` | firmware board id |
| `soc` | `"rk3188"` | SoC family |
| `fw` | `"t3-linux-0.1"` | firmware version |
| `hwid` | `"ff72f3bb…"` | hardware identity (eFuse-derived on the T3) |
| `mac` | `"00:0f:ff:81:ca:08"` | primary NIC MAC |
| `driverVersion` | `"84"` | last driver version the device saw (echoed back) |
| `display` | `{"w":1280,"h":800}` | panel dimensions |
| `net` | `{"link":"ethernet"}` | active link: `ethernet` \| `wifi` |
| `power` | `{"source":"wall"}` | `wall` \| `poe` \| `battery` |
| `caps` | `{"display":true,"touch":true,"audio":true,"intercom":false}` | capabilities |
| `features` | `"intercom,walkup"` | licensed features (when licensed) |

Unknown manifest fields MUST be ignored (forward-compat). The driver surfaces
`model` / `mac` / `net.link` / `power.source` as read-only properties.

Example:
```json
{"t":"hello","mac":"00:0f:ff:81:ca:08","fw":"t3-linux-0.1","intercom":false,"hwid":"ff72f3bb3ca1f145f4daafcb","secret":"…","sku":"mmk-t3","manifest":{"sku":"mmk-t3","model":"Control4 T3-7","board":"t3-7","soc":"rk3188","fw":"t3-linux-0.1","mac":"00:0f:ff:81:ca:08","display":{"w":1280,"h":800},"net":{"link":"ethernet"},"power":{"source":"wall"},"caps":{"display":true,"touch":true,"audio":true,"intercom":false}}}
{"t":"cmd","cmd":"playpause"}
{"t":"vol","level":35}
{"t":"source","id":"118"}
{"t":"button","id":3}
```

---

## Driver → Device

### `state` — the full now-playing + room snapshot (idempotent; send whole object)
```json
{
  "t":"state",
  "proto":1,
  "room":"2419",
  "power":true,
  "playing":true,
  "mediaType":"media",
  "title":"Be By You",
  "artist":"Luke Combs",
  "album":"The Way I Am",
  "artUrl":"https://is1-ssl.mzstatic.com/.../600x600bb.jpg",
  "source":{"id":"3058","name":"Apple Music"},
  "volume":34,
  "muted":false,
  "duration":0,
  "position":0,
  "meta":[{"label":"Format","value":"FLAC"},{"label":"Sample Rate","value":"44100"}],
  "rotation":-1,
  "layout":-1,
  "btnStyle":-1,
  "bg":-1,
  "brightness":-1,
  "dimSec":-1,
  "dimLevel":-1,
  "showTitle":true,
  "showArtist":true,
  "showInfo":true,
  "showProgress":true,
  "canPause":true,"canStop":false,"canNext":true,"canPrev":true,
  "canThumbsUp":false,"canThumbsDown":false,
  "canShuffle":false,"canRepeat":false,
  "buttons":[{"id":1,"label":"Lights","on":true,"color":"00ff00","icon":"Lights","offIcon":""}]
}
```

Notes:
- `proto`: protocol version (currently **1**); see Versioning.
- `artUrl`: album art (https CDN) for audio sources, else the source device's
  Control4 icon resolved to `http://<controller>/.../icon.png`. The device fetches
  it (TLS for https, plain for http) and decodes JPEG (album art) or PNG (icons).
  Empty → placeholder.
- `mediaType`: `media` when track metadata exists, else `other` (powered, no
  metadata) or `off`.
- `mediaTypeV2`: richer source classification from the media-info block
  (`mediatypeV2`, falling back to legacy `mediatype` — e.g. `GENERIC_MEDIA`, `SONG`).
  `""` = unknown. Forward-compat hint for the UI; absent/empty ignored.
- `source.name` prefers a reported "current app" (e.g. YouTube) over the device
  name. (A `sources` list used to ride along for an on-screen source picker; the
  panel has no picker, so the driver no longer sends it.)
- `meta`: ordered track-info `label`/`value` pairs for the (i) panel.
- `rotation`: display orientation `0–3` (0 landscape … 3 portrait-flipped), or
  `-1` = leave the device/web setting. `layout`: `0` Cover / `1` Fit / `2` Compact,
  or `-1` = leave as-is. The device applies layout live; orientation may reboot.
- `btnStyle`: keypad "on" appearance — `0` LED accent bar (physical-keypad look),
  `1` Filled tile (app look), or `-1` = leave the device/web setting. Applied live.
- `bg`: background gradient preset — `0` Navigator (violet→blue), `1` Ocean, `2` Dusk,
  `3` Graphite, or `-1` = leave the device/web setting. (The room's real Navigator
  wallpaper isn't exposed to drivers, so the device renders a gradient preset.)
- `brightness`: active screen backlight `0–100`, or `-1` = leave the device/web setting.
- `dimSec`: dim the backlight after this many idle seconds (`0` = never dim), or `-1`
  = leave. Never dims while media is playing or during a call.
- `dimLevel`: idle backlight `0–100` after `dimSec` (`0` = screen fully off; a touch
  wakes it), or `-1` = leave. All three apply live (no display rebuild). Persisted to NVS.
- `showTitle`/`showArtist`/`showInfo`/`showProgress`: bool, dealer-configurable
  now-playing element visibility (track title, artist/album line, the (i) button,
  progress bar + time). Absent = shown. (Progress also requires a known `duration`.)
- `canPause`/`canStop`/`canNext`/`canPrev`/`canThumbsUp`/`canThumbsDown`: transport
  capabilities the source declares (driver reads the source's `<Dashboard>`). The
  device renders only the supported controls — Stop instead of play/pause when
  `!canPause && canStop`, skip hidden when `!canNext`/`!canPrev`, and 👍/👎 when the
  thumbs caps are set. Absent → full play/pause + skip (legacy behavior).
- `canShuffle`/`canRepeat`: source supports shuffle/repeat (declared as CUSTOM
  `ShuffleOn`/`Off`, `RepeatOn`/`Off` in the Dashboard). The device shows toggle
  buttons in the quick-actions panel that send `cmd` `shuffle`/`repeat` (a pure
  toggle to the source).
- `shuffleOn`/`repeatOn`: **real** current state. The driver reads it from the
  digital-audio session device (the `<deviceid>` in room 1031, e.g. `100002`
  `control4_digitalaudio`) variable 1003 (`room_queue_settings`), keyed by room id.
  The device reflects it as the button highlight (with a brief optimistic hold on
  tap so feedback is instant while the source's state catches up).
- `buttons`: the CONFIGURED keypad buttons — `id` (Control4 button id; the device
  echoes it back in a `button` message), `label` (on-screen text), `on` (LED lit),
  `color` (`rrggbb` lit color), `icon` (on-state icon name or ""), `offIcon` (icon
  shown when `on` is false; "" → use `icon` in both states). Icon names match the
  driver's per-button On/Off Icon catalog.
  The list carries **only buttons that have been set up** (given a label or linked to
  a device), so it is normally SHORTER than the panel's `caps.buttons` and its ids
  are sparse — `caps.buttons` is how many the hardware can show, not how many exist.
  Send nothing and the panel shows no button tiles at all; an entry with an empty
  `label` is ignored rather than drawn as "Button N".
- `duration`/`position`: seconds; `0` = unknown. `duration` 0 → hide progress. The
  device runs a local 1 Hz clock for the bar, but **seeds/resyncs** it from `position`
  whenever the driver reports a non-zero value (fresh track, seek, or joining
  mid-playback). `position` 0 → keep the local clock (best-effort; many sources don't
  expose elapsed time).

### `identify`
```json
{"t":"identify"}
```
Device flashes its screen so an installer can tell which physical unit this is.

### `announce` — play a Control4 announcement
```json
{"t":"announce","text":"Dinner is ready","chime":true}
```
Device plays a short notification **chime out its speaker** and shows `text` as a
top banner for a few seconds. Both fields optional: `text` "" / absent → chime only;
`chime` absent → defaults `true`; `chime:false` → banner only (no sound). Overlapping
chimes are ignored (one at a time). Sent by the driver's `PlayAnnouncement`
programming command. (First step of Phase-3 audio — chime + on-screen text; spoken
TTS / streamed audio is a later addition.)

### `pong`
```json
{"t":"pong"}
```

### `rooms` — multiroom list (reply to `getrooms`)
Every audio room, which are grouped with this room (share our media session), and
what each is doing. Built on demand from a project room enumeration + the digital-audio
device's `QUEUE_STATUS_V2` (var 1006, all queues house-wide). Forward-compatible: a
device that doesn't render it ignores it.
```json
{"t":"rooms","list":[
  {"id":"2434","name":"Office","grouped":true,"playing":true,"active":true},
  {"id":"2420","name":"Kitchen","grouped":false,"playing":false,"active":false}
]}
```
- `grouped` — in **this** room's session (the add-rooms checkbox state); our own room is always `true`.
- `playing` — that room's queue state is `Play`.
- `active` — that room is in some session (playing or paused).

Tapping a room row sends `grouproom` (join/leave). Our own room can't be un-grouped from here.

### `favorites` — this room's favorite tiles (reply to `getfavorites`)
The room's navigator favorite tiles (the user-configured music favorites), read from the
UI Configuration agent (verified, no navigator identity). Each entry:
`id` (opaque favorite id — send it back in `favorite` to play), `title` (display name),
`image` (artwork URL, may be empty), `kind` (`stream` | `broadcast`). Forward-compatible:
a device that doesn't render it ignores it. Tapping a tile sends `favorite` with its `id`.
```json
{"t":"favorites","list":[
  {"id":"70F6945B-...","title":"Mike DeLuca's Station","image":"https://.../1024x1024sr.jpg","kind":"stream"},
  {"id":"8F0FD8C2-...","title":"Electronic Station","image":"https://.../1024x1024sr.jpg","kind":"stream"}
]}
```
> `kind:"stream"` tiles (media-service stations/playlists) play by selecting their source
> (exact station play is navigator-gated); `kind:"broadcast"` tiles play the exact media
> item. Both are enacted driver-side from the tile `id` — the device just sends the id.
> Only media favorites are returned; category-launcher tiles are filtered out.

#### How a favourite is played (driver-side)

`kind:"broadcast"` tiles play exactly via `SELECT_AUDIO_MEDIA {mediaid}`.

`kind:"stream"` tiles (media-service stations/playlists) play exactly via the media
service's **`Play Item`** command — verified live on Apple Music, 2026-08-13. This
replaces the old belief that exact stream play was navigator-gated and unreachable:

```
command: "Play Item"
target:  the service's DRIVER device — NOT the media_service proxy the favourite
         names. Sending to the proxy is silently ignored. The proxy carries an input
         binding of class MediaService whose PROVIDER is the driver device; ask
         Control4 for that bound provider rather than guessing at device numbering.
params:  Item    = base64(JSON, below)
         Room    = <roomId>
         Shuffle = "Off" | "On"
```

`Item` is **base64-encoded JSON**, not a bare id — passing the raw id or the catalog
href is accepted and silently does nothing. Composer stores a fat version (six artwork
URLs and an `actions_list`) for its own picker UI; the service only needs:

```json
{ "default_action": "PlayStation",
  "href":     "/v1/catalog/us/stations/ra.985488606",
  "id":       "ra.985488606",
  "itemType": "stations",
  "mediaKind":"audio",
  "title":    "Electronic Station" }
```

Every field comes from the favourite we already parse: `href`/`id`/`itemType` are the
flattened `context` on the favourite's own `path`
(`…/mediaservicefavorite/<src>?context=href~…~id~…~itemType~…`), and `title` from
`<title>`.

Falls back to selecting the source when a favourite carries no catalog context, or the
service exposes no `Play Item`. Note that selecting a streaming source must be done
with a room-level `SELECT_AUDIO_DEVICE`: `DEVICE_SELECTED` to the source alone leaves
the room powered OFF.


### `license` — relay a license token (for offline keypads)
```json
{"t":"license","token":"nvl1.<b64url payload>.<b64url sig>"}
```
The driver fetches the keypad's license from nuvoxel (using the `hwid`/`secret` the
device advertised in `hello`) and pushes it here. The device verifies the token
**offline** against its firmware-baked public key (hardware-bound to its `hwid` +
`sku`), persists it in NVS, and replies `{"t":"license","status":"ok|error"}`. This
lets a keypad with no internet of its own still get licensed via the C4 driver.

### `reboot` — restart the device
```json
{"t":"reboot"}
```
Device reboots (`esp_restart`). Sent by the driver's `RebootDevice` programming
command. No fields.

### `ota` — check & update firmware
```json
{"t":"ota","url":"https://example.com/firmware/mmkeypad.bin"}
```
Device pulls the image over HTTPS (TLS verified via the cert bundle) and flashes it
with `esp_https_ota`, reporting progress back as `ota` messages. It compares the new
image's embedded version against the running one and **only re-flashes if they differ**
(otherwise reports `status:"uptodate"`); on success it reboots into the new image.

---

## Intercom / SIP — same **:6700** channel (Phase 3)

Intercom control is **multiplexed onto the same :6700 socket** as the now-playing/
keypad protocol — there is **no second TCP channel to the device**. The device runs a
SIP user agent (`esp_rtc`); these messages provision its SIP account and bridge call
state. See [PHASE3-INTERCOM-SIP.md](PHASE3-INTERCOM-SIP.md).

**One driver, one device link.** The intercom is a **sub-proxy** (`intercomproxy`,
binding 5003) of the single multi-proxy `NuVoxelKeypad.c4z` driver — alongside the
primary `keypad` proxy (binding 5002), both hosted in `driver-keypad/`. The
driver owns the device's one `:6700` link and dispatches the
`sip`/`call`/`sipstate`/`callstate` messages straight to/from `intercom.lua`
(`ReceivedFromProxy`/`ReceivedFromNetwork` on the same connection) — no relay,
no second driver, no control-relay class. The device protocol below is
unchanged.

On the device, `net.c` routes `sip`/`call` to `sip.c` (`sip_handle`), and `sip.c`
sends `sipstate`/`callstate` back via `net_send_line` over the same socket. SIP only
runs if audio (ES8311) came up at boot; otherwise these messages are ignored.

### Driver → device

| `t`    | Fields | Meaning |
|--------|--------|---------|
| `sip` | `server` (str), `port` (int, def 5060), `transport` ∈ `udp\|tcp\|tls`, `user` (str), `pass` (str), `autoAnswer` (bool, def true) | Provision SIP creds → device registers via `esp_rtc`. Re-sent on change. |
| `callcfg` | `autoAnswer` (bool), `monitor` (bool), `mute` (bool), `playDoorChime` (bool) — any subset | Live call behavior (no re-register). All follow the **native intercom proxy settings** (driver polls its own `GET_DEVICE` props). `monitor` = silent auto-answer; `mute` = stop sending mic audio (`MUTE_CALL`); `playDoorChime` = door-station calls ring the doorbell chime (else the normal ring). |
| `sipvol` | `speaker` (0–100), `ringer` (0–100), `mic` (0–100) — any subset | Intercom levels from `SET_SPEAKER_VOLUME`/`SET_RINGER_VOLUME`/`SET_MICROPHONE_GAIN`. Speaker→DAC %, ringer→chime %, mic→ES8311 ADC gain. |
| `endpoints` | `list` (array of `{name, user, door, group, actions}`) | Callable intercom targets for the on-screen **Intercom picker** — rooms, door stations, and groups (Everyone / call groups; `group=true`). `user` is the SIP username (endpoint) or group name; the device places `esp_rtc_call(user)` on tap (calling a group broadcasts). Excludes self. Re-pushed on link-up. `actions` (optional, 0–2 of `{id, label}`) are that endpoint's own door actions — typically two relays (door + gate) — rendered as buttons on its call screen. The panel shows exactly what it is told and never infers an action from a target's name or kind; an entry with no `label` is dropped. |
| `doorstations` | `users` (array of SIP usernames) | The intercom endpoints the Director reports as door stations (`isDoorStation`, via the proxy `GET_DEVICE_LIST`). A call whose peer is in this set is a **doorbell** → doorbell chime + manual answer, never auto-answered/monitored. Vendor-neutral (native DS2, UniFi/Chowmain, 2N, …); no peer-name heuristics. Re-pushed on link-up. |
| `call` | `action` ∈ `accept\|end\|reject\|start`, `remote` (str, for `start`), `session` (opt), `monitor` (bool, opt) | Call control, mapped from `intercomproxy` commands. Unknown actions ignored. (`pause`/`resume` are deprecated in the proxy protocol and removed.) |

### Device → driver

| `t`         | Fields | Meaning |
|-------------|--------|---------|
| `sipstate`  | `registered` (bool), `user` (str, opt) | SIP registration changed → driver's `SIP Registration` property + proxy `CURRENT_STATE`/`SIP_USERNAME_CHANGED`. |
| `cloudreq` | `id` (int), `path` (str, `/api/v1/fw/…`), `body` (str, raw JSON) | **Offline relay.** The device asks the driver to POST one of ITS cloud endpoints on its behalf, for a panel with no route of its own. The driver authenticates as the device (it already holds `hwid`/`secret` from `hello`) and answers with `cloudresp`. `path` is constrained to the device API — the driver must not become a general-purpose proxy for whatever is on the socket. The device only sends this when its OWN request failed, so a panel with internet never uses it. |
| `fwget` | `id` (int), `url` (str), `off` (int), `len` (int ≤ 4096) | **Offline relay.** One byte-range of a firmware image, answered with `fwdata`. The device PULLS chunk by chunk, so the transfer is flow-controlled and neither side buffers an image — the device writes each chunk straight into its OTA partition and verifies the sha256 over the whole thing before switching boot. Chunks are small because the device's line buffer is 8 KB. The driver only serves our own catalogue/blob hosts. |
| `callctl` | `action` ∈ `mute\|door`; `on` (bool, for `mute`); `id` (str) + `remote` (str, for `door`) | On-screen call controls. `mute` reports the panel's mic-mute back up so the driver mirrors it to the proxy (`NOTIFY.Mute_Audio_Changed`) and Navigator's call UI agrees — it is the return path for `callcfg.mute`/`MUTE_CALL`. `door` fires one of the door's advertised `actions` (`id` as given in `endpoints`); Control4's intercom proxy has **no** door/relay command and `GET_DEVICE_LIST`'s `device_props` carry no relay info, so the driver raises a programming **event** (`Door Action 1`/`2`, with `LAST_DOOR_PEER` set) for the dealer to wire to a relay, lock or macro. |
| `cloudresp` | `id` (int), `code` (int, 0 = transport failure), `body` (str), `err` (str, opt) | Reply to `cloudreq`. |
| `fwdata` | `id` (int), `off` (int), `len` (int), `b64` (str), `eof` (bool), `err` (str, opt) | Reply to `fwget`: base64 of that range. |
| `callstate` | `event` ∈ `incoming\|outgoing\|accepted\|ended\|rejected`, `remoteAor` (str), `session` (opt) | SIP call state → driver emits the matching `intercomproxy` notification (`INCOMING_CALL`, `CALL_ACCEPTED`, `CALL_ENDED`, …). |

```json
{"t":"sip","server":"192.168.1.50","port":5060,"transport":"udp","user":"1012","pass":"…","autoAnswer":true}
{"t":"sipstate","registered":true,"user":"1012"}
{"t":"callstate","event":"incoming","remoteAor":"1001@192.168.1.50"}
```

A C4 announcement is a one-way **monitor** call: with `autoAnswer` the device
accepts the INVITE and plays the incoming RTP out the speaker. Unknown `t` ignored.

---

## Versioning

`proto` (currently **1**) is carried in every `state` message. Bump it on breaking
changes. Fields are additive where possible and unknown `t` types / unknown fields
MUST be ignored, so a newer driver against older firmware (or vice-versa) degrades
gracefully rather than breaking. (A strict version gate is not yet enforced on
either side — `proto` is sent so one can be added later.)
