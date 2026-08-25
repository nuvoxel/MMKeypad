# MMKeypad — Architecture

A two-part system that turns an **lcdwiki 2.8" ESP32-S3 display board** into a
now-playing display + transport controller (and, later, an intercom/announcement
endpoint) for a **Control4** room, driven by a custom **Control4 `.c4z` Lua
driver**.

Inspired UX-wise by [`esphome-media-player`](https://github.com/jtenniswood/esphome-media-player),
but with **no Home Assistant / ESPHome** — the media state and control come from
Control4 directly.

```
 ┌──────────────────────────┐    TCP :6700, newline JSON       ┌─────────────────────┐
 │ Control4 Director         │  ──── connects out (binding) ──► │ ESP32-S3 (server)   │
 │  NuVoxelKeypad.c4z (Lua)  │  ──── state push ──────────────► │  LVGL now-playing   │
 │   • NetConnect(6001,6700) │  ◄─── commands ───────────────   │  + touch controls   │
 │   • RoomGetId / GetDevice │        art URL ► HTTP GET ─────► │  status web page    │
 │     Variable / SendToDev  │  ◄──────────────── from Director │                     │
 └──────────────────────────┘                                  └─────────────────────┘
   one driver instance per device · device IP comes from the Connections binding
```

See [PROTOCOL.md](PROTOCOL.md) for the exact wire format — it is the contract
between the two halves and should be kept authoritative. This doc is
design-rationale only; for the per-half reference docs see
[`driver-keypad/README.md`](driver-keypad/README.md) (driver internals),
[`firmware-idf/README.md`](firmware-idf/README.md) (firmware/board internals),
and PROTOCOL.md (wire format).

---

## Native discovery & binding (SDDP) — implemented

The device is **discovered via SDDP** and **bound on the Connections page**; the
driver then connects out to it (device = server). No IP config on the device.

- **Device advertises over SDDP** (multicast `239.255.255.250:1902`): periodic
  `NOTIFY ALIVE` with `Type:`/`Manufacturer:`/`Model:`/`Driver:`/`Host:` headers,
  responds to `SEARCH`, re-announces before `Max-Age`. Composer auto-discovers it
  (the "Discovered"/"Available Devices" lists), the installer **Identifies** it,
  and **binds it to a driver instance on the Connections page** — which is how
  device→driver mapping is resolved when there are many units, with no IP config
  on the device.
- **Direction flips: the device becomes the TCP server; the driver connects out.**
  The driver declares a **network binding** (`<connection>` id `6001`, `type 4`,
  `classname TCP`, port e.g. `6700`, auto/keep/monitor) and, on bind, reads the
  device address from the binding and dials it:
  - `OnNetworkBindingChanged(6001, true)` → `C4:GetBindingAddress(6001)` /
    `C4:GetDiscoveryInfo(6001)` (`.ip/.host/.uuid/...`) → `C4:NetConnect(6001, 6700, "TCP")`
  - `OnConnectionStatusChanged(6001, port, "ONLINE")` gates sends
  - `ReceivedFromNetwork(6001, port, data, from)` buffers/parses (same line-JSON
    protocol, just over the binding instead of a hosted server)
  - register `OnSDDPDeviceStatus` (system event #78) for reliable online/UUID.

> ⚠ Verification gaps (flagged like the proxy work): the exact **SDDP packet text**
> and the **`<sddp>` driver.xml discovery element** are not fully public (community
> reverse-engineering + license-gated SDK). Validate the ESP32 responder with
> Wireshark on UDP/1902 against the cores/doorstations/GlassEdge units already on
> the network. The network-binding Lua APIs above ARE officially documented.

Until then, the current "device connects out to a configured controller IP"
path stays as the working fallback.

## Cover art

Phase 1: driver sends `artUrl`; CYD HTTP-GETs and decodes. Simplest, least load
on the Director.

Fallback (if Director art URLs aren't LAN-reachable from the CYD, e.g.
`controller://` forms): driver fetches via `C4:url():Get()`, optionally
downscales, and sends base64 over the socket as `artB64`. Heavier; only if needed.

---

## Phasing

1. **Phase 1 — Now-playing link.** Server + room read + `state` push; CYD shows
   art/title/artist. Read-only. Proves the hardest part (C4 media plumbing).
2. **Phase 2 — Control + config.** Transport/volume/mute/source commands; web
   config portal + room picker; reconnect hardening.
3. **Phase 3 — Announcements (stretch, hardware-gated).** See below.

---

## Phase 3: intercom / announcements — feasibility

Goal inspiration: [`esphome-intercom`](https://github.com/n-IA-hane/esphome-intercom).

**Good news: the hardware can do it.** This board has an **ES8311 codec + FM8002E
amp + analog MEMS mic on I2S** — the same codec family as the ESP32-S3-BOX, which
is exactly what `esphome-intercom` targets. With the S3's 8MB PSRAM, ESP-SR's AFE
(echo cancellation, noise suppression, VAD) is feasible. So the firmware side of a
two-way intercom is realistic. **The hard part is now the Control4 side**, not the
hardware.

The Control4 side, in rough order of effort:
1. **On-screen notices (no audio) — easiest, ship first.** Driver detects an
   announcement/notification event and pushes a `notice` message (text + optional
   local chime played through the ES8311) that the screen shows.
2. **One-way announcement audio.** Control4's Announcements agent plays audio
   through its **audio matrix to rooms**, not to arbitrary network endpoints, so
   the driver likely has to source/stream the clip itself (e.g. push a TTS/clip
   **URL** the device fetches and plays). Medium effort.
3. **Two-way intercom — the C4 `intercomproxy` proxy. Shipped (driver side).**
   Control4 intercom is **SIP**-based (Director runs a SIP registrar; T3/T4
   touchscreens are SIP endpoints). `driver-keypad/` already hosts the
   **`intercomproxy`** as a sub-proxy (binding 5003) of the multi-proxy
   `NuVoxelKeypad.c4z` driver, alongside the primary `keypad` proxy (5002) —
   Control4 treats the device as a **native intercom endpoint**, handing the
   driver call-control commands — `START_CALL`, `INCOMING_CALL`,
   `ACCEPT_CALL`/`REJECT_CALL`, `END_CALL`, `PAUSE/RESUME_CALL`,
   `PLAY_DOOR_CHIME`, `SET_SPEAKER_VOLUME`, `SET_MICROPHONE_GAIN`, DND/camera/
   ringer settings — and the driver notifies back (`OUTGOING_CALL`,
   `CALL_ACCEPTED`, etc.). C4 does the SIP signaling; we drive audio via the
   ES8311. The remaining work is the **on-device audio/RTP** in firmware (a
   real SIP/RTP media path via `esp_rtc`) — see
   [PHASE3-INTERCOM-SIP.md](PHASE3-INTERCOM-SIP.md) for the current status.
   NOTE: the `keypad`/`button` proxies are physical-button/LED interfaces with no
   media metadata — not used here; now-playing always comes from reading the room.

The socket/protocol is forward-compatible (unknown `t` ignored), so `notice` and,
later, an audio channel can be added without breaking v1.
