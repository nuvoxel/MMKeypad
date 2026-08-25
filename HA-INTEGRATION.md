# Home Assistant integration — scoping

Status: **proposed / not started.** This scopes a Home Assistant (HA) integration
for the NuVoxel keypad so it can be sold and run as a direct-to-consumer HA SKU,
sitting alongside the Control4 DriverWorks driver.

## Goal

A homeowner running Home Assistant can add a NuVoxel keypad and have it:

1. Show now-playing for a room's media and drive transport / volume / source.
2. Fire its programmable buttons as HA events (automations, scenes).
3. Play HA announcements out the speaker.
4. (Phase 3) Two-way intercom.

…with the same download → flash → activate → "get on with life" flow as the C4
path, and the same licensing (Base free, Pro trial → paid).

## Prior art & inspiration

Two philosophies dominate HA wall displays today:

- **Native device (custom UI, auto-discovered).**
  [esphome-lvgl / Guition ESP32-P4](https://github.com/jtenniswood/esphome-lvgl/tree/main/guition-esp32-p4-jc1060p470)
  is a 7" ESP32-P4 panel (same class as our P4 SKUs) running a **YAML-defined
  LVGL UI** on ESPHome, integrating via the **ESPHome native API** — HA
  auto-discovers it, no custom component. Validates the hardware + the
  purpose-built-UI direction, and shows how much HA users value zero-config
  discovery. We differ in that our firmware is hand-written C/LVGL with SIP
  intercom + our own licensing, which ESPHome can't express — but the discovery
  expectation is the lesson (→ approaches C/D).

- **Browser kiosk (generic Lovelace dashboard).**
  [TouchKio](https://github.com/leukipp/touchkio) (used in
  [The Stockpot's PoE wall-display build](https://www.thestockpot.net/videos/home-assistant-wall-display))
  is an Electron kiosk on a Pi that renders the **HA Lovelace dashboard** and
  exposes the *device's own* hardware back to HA via **MQTT discovery**
  (brightness, screen power, reboot, battery, temperature, screenshots…). Two
  takeaways we adopt: (1) **MQTT discovery is the clean, component-free way to
  surface a panel's hardware + telemetry** (→ approach D), and (2) PoE +
  flush-mount + "phone-free" is exactly our in-wall positioning.

**Where we sit:** not a generic browser dashboard — a purpose-built media/room
keypad with SIP intercom and device-owned licensing. So we keep the custom LVGL
UI, expose the *device* via MQTT discovery (TouchKio-style), and bridge the
*media_player* via a thin custom component (ESPHome-native-API-style discovery is
noted as the future "zero-component" option). ESP boards can't run a browser, so
the kiosk approach isn't applicable to our hardware anyway.

## Design principle: the device is ecosystem-agnostic

The keypad is **the server** on TCP `:6700`; it speaks its own newline-delimited
JSON protocol ([PROTOCOL.md](PROTOCOL.md)); the *driver dials in*. The Control4
driver is one such client (Lua, in Composer). Licensing, identity, OTA, and the
activation gate all live **in the device**, not in the driver — the driver only
surfaces + relays.

That means **HA does not require a firmware rewrite.** The cleanest integration
mirrors the C4 driver: an HA **custom component** dials the keypad's `:6700`,
speaks the *existing* protocol, and translates to/from HA. The keypad never needs
to know whether the peer is Control4 or Home Assistant.

### Approaches considered

| | A. Firmware WS-API client | B. Custom component bridges `:6700` | C. Firmware speaks ESPHome native API | D. MQTT discovery |
|---|---|---|---|---|
| HA-specific code | in the C firmware | Python custom component | in the C firmware (protobuf + noise) | firmware publishes discovery topics |
| Firmware change | new WS transport + HA state mapping | **none** | large (reimplement ESPHome's API server) | moderate (MQTT client + discovery) |
| HA setup friction | needs a token + component | **install a component** | **zero — auto-discovered** | **zero — auto-discovered** |
| Ecosystem-agnostic core | ✗ | ✓ | ✗ (bakes ESPHome in) | ✓ (MQTT is generic; we already run a broker) |
| Can do `media_player` display + control | ✓ | ✓ (rich) | ✓ | ✗ (HA has no MQTT `media_player`) |
| Good for device hardware/telemetry/buttons | ✓ | ✓ | ✓ | **✓ (its sweet spot)** |

**Recommendation — layered (D + B):**

- **D. MQTT discovery for the device itself** (backlight/brightness, screen power,
  reboot, online, room/presence, fw version, **license + trial**, and each
  programmable **button** as an event) — the TouchKio pattern. Zero HA config,
  HA auto-creates the entities, reuses the nuvoxel broker, and keeps the core
  ecosystem-agnostic (MQTT isn't HA-specific). This alone makes the keypad useful
  in HA (its buttons become triggers, its hardware becomes controls) with **no
  component to install**.
- **B. A thin custom component for the one thing MQTT can't model — the
  `media_player` bridge** (read a room's player, drive transport/volume/source,
  now-playing). This is the C4-driver-equivalent, in Python.

C (ESPHome native API) gives the best *native* UX — HA discovers a fully-native
device with no component — but reimplementing ESPHome's protobuf+noise API server
in our custom C/LVGL/SIP firmware is a large lift and couples the firmware to
ESPHome's protocol. Not worth it while D covers discovery-free device integration
and B covers media. Revisit only if we ever want the keypad to need *nothing* HA-
side. A remains rejected (all cost, no discovery benefit over C).

## The transport seam (why B is low-risk)

On the firmware side, every transport feeds the UI through one struct
([net.h](firmware-idf/main/net.h) `net_callbacks_t`): `on_connect`,
`on_disconnect`, `on_state(media_state_t)`, `on_identify`, `on_display_change`,
`on_announce`, `on_endpoints`. The device already produces/consumes the protocol
that populates these. The HA component just becomes the thing on the other end of
the socket — no new firmware seam.

## Component design — `custom_components/nuvoxel_keypad/`

A standard HA integration (HACS-installable, later HA-core-submittable):

- **`config_flow.py`** — discovery + setup.
  - Discover keypads via **mDNS/SDDP** (the device advertises today via
    `sddp_start()`; add an mDNS `_nuvoxel._tcp` record so HA's zeroconf finds it),
    or manual IP entry.
  - Bind the keypad to a **room**: an HA **area** (preferred) or a specific
    `media_player` entity. The keypad represents one room, like the C4 model.
- **`coordinator.py`** — one long-lived TCP client to the keypad `:6700`.
  - **Inbound (HA → keypad):** subscribe to the bound `media_player` state; on
    change, build a `state` message and send it (mirrors the C4 driver's
    `PushState`). Handle `hello`/`manifest` (identity + capabilities), reply to
    `ping`.
  - **Outbound (keypad → HA):** translate `cmd` / `vol` / `mute` / `source` into
    `media_player` service calls; fire `button` as an HA event.
- **`__init__.py` / entities:**
  - The keypad as an HA **device** (from its manifest: model, MAC, fw, room).
  - A **`sensor`** (or device attributes) exposing license tier / **trial · N
    days left** / fw version / online — sourced from the same license relay.
  - Optionally a `button`/`event` entity per programmable key.
- **License relay** — the component has WAN, so it fetches this keypad's license
  from nuvoxel (`POST /api/v1/fw/entitlement` with the device identity from
  `hello`) and pushes the token down the link, exactly like the C4 driver's
  `FetchAndPushLicense`. Needed only for keypads without their own internet; a
  keypad that reaches the cloud licenses itself. The component also **self-
  activates** (`POST /api/v1/driver/activate`) so HA installs show up as usage.
- **`manifest.json`** — domain `nuvoxel_keypad`, zeroconf discovery, iot_class
  `local_push`, requirements.

## Protocol ↔ Home Assistant mapping

| Keypad protocol | Direction | Home Assistant |
|---|---|---|
| `hello` (mac, fw, `manifest`, hwid/secret/sku) | keypad → | device registry entry; identity for the license relay |
| `state` (room, power, playing, title/artist/art, volume, muted, source, sources, buttons, `driverVersion`, `proto`) | → keypad | built from the bound `media_player` attributes (`state`, `media_title`, `media_artist`, `entity_picture`, `volume_level`, `is_volume_muted`, `source`, `source_list`); `room` = area name |
| `cmd` (play/pause/next/prev/stop/…/roomoff) | keypad → | `media_player.media_play_pause`, `media_next_track`, `media_previous_track`, `media_stop`; `roomoff` → `media_player.turn_off` (area) |
| `vol` (level 0–100 \| dir up/down) | keypad → | `media_player.volume_set` / stepped `volume_up`/`down` |
| `mute` | keypad → | `media_player.volume_mute` |
| `source` (id) | keypad → | `media_player.select_source` |
| `button` (id) | keypad → | fire event `nuvoxel_keypad_button` (device_id, button) → automations |
| `announce` (text, chime) | → keypad | driven by an HA automation/`tts` targeting the keypad (see below) |
| `ota` / `license` | keypad ↔ | surface as device attributes; license relay result |

Thumbs / shuffle / repeat and full source browse map to fewer HA primitives than
C4's MSP — v1 exposes what `media_player` supports; richer browse is a later item.

## Licensing — already ecosystem-agnostic

No new licensing work. The keypad registers, pulls its Base/Pro-trial license,
shows the **activation gate** when unlicensed and **"Trial (Pro) · N days left"**
on its own screen — identical whether paired with C4, HA, or nothing. The HA
component only *surfaces* it (a sensor) and *relays* it for offline keypads. The
paywall stays in activation, so the freely-downloadable HA component leaks no IP.

## Announcements

HA-native and cleaner than C4: an automation calls `tts.speak` (or
`media_player.play_media`) targeting the keypad's speaker, or the component
exposes a `notify`/service that emits an `announce` message. v1: document the
automation pattern; v2: a first-class `notify` entity.

## Intercom (Phase 3, separate)

Two-way intercom does **not** go through HA. Keep the custom C/LVGL + SIP
firmware; the keypad's SIP UA registers directly to an **external Asterisk/PBX**,
bypassing HA entirely (HA has no real SIP intercom). This is the same SIP stack
as the C4 intercom path — see [PHASE3-INTERCOM-SIP.md](PHASE3-INTERCOM-SIP.md).
The HA component is unaware of it.

## Phased plan

Leads with the **zero-component MQTT win** (immediate HA value), then adds the
media bridge.

1. **P0 — MQTT discovery (device), no component.** Firmware MQTT client publishes
   HA discovery for: backlight/brightness (`light`/`number`), screen power
   (`switch`), reboot (`button`), online (`binary_sensor`), room + fw + IP +
   **license/tier + trial days** (`sensor`), and each programmable **button** as
   an `event`/`device_trigger`. HA auto-creates everything; the keypad's buttons
   become automation triggers and its hardware becomes HA controls with **no
   install**. Reuses the nuvoxel broker (or the user's HA Mosquitto). *~medium
   (firmware MQTT + discovery payloads).*
2. **P1 — Media component MVP.** Custom component skeleton, config_flow (manual
   IP), TCP client to `:6700`, `hello`/`manifest`, push `state` from one bound
   `media_player`, handle `ping`. Keypad shows live now-playing under HA. *~small.*
3. **P2 — Media control.** `cmd`/`vol`/`mute`/`source` → `media_player` service
   calls. Full control of the room from the keypad. *~small.*
4. **P3 — Discovery + device linking.** mDNS `_nuvoxel._tcp` in firmware + HA
   zeroconf config_flow; tie the P0 MQTT device and the component together in the
   device registry (one HA device). *~small (firmware: add the mDNS record).*
5. **P4 — License relay + activation.** Component fetches + pushes the license for
   offline keypads; self-activation telemetry (already surfaced via P0 MQTT for
   online keypads). *~small (ports the C4 relay to Python).*
6. **P5 — Packaging.** HACS repo, docs, then submit to HA core. *Public repo — see
   the open-source boundary.*
7. **Phase 3 — Intercom** via external Asterisk. Independent of the above.

> Note: a keypad on HA that only wants **hardware controls + button triggers**
> (no media display) is fully served by **P0 alone** — no component. The media
> bridge (P1–P2) is additive.

## Open decisions

- **Room binding:** HA **area** (auto-picks the area's media_player) vs a single
  chosen `media_player` entity. Area is more "HA-native"; entity is simpler. Lean
  area with an entity override.
- **Auth to HA:** the media component runs *inside* HA (state access built in) and
  MQTT uses the broker's own creds — so **no long-lived token** is needed, the
  ugly part of a firmware HA-client (A/C).
- **Which broker for P0:** the nuvoxel broker (we already run it) vs the user's HA
  Mosquitto add-on. Prefer configurable, default to the HA add-on for locality.
- **Announcement UX:** documented automation (v1) vs a `notify` entity (v2).
- **Repo:** the HA component is public (community can contribute), same boundary
  as the driver + `nuvoxel_device`; firmware/backend/keys stay private.
- **mDNS vs SDDP:** SDDP is Control4-specific; add a parallel mDNS record for HA
  zeroconf rather than making HA speak SDDP.
- **ESPHome native API (C):** deferred, not rejected — it's the only path to a
  fully native, component-free *media* device, but a large firmware lift. Revisit
  if D+B friction proves too high.

## Non-goals (v1)

- No firmware WebSocket/HA client (approach A) — the device stays protocol-neutral;
  ESPHome native API (C) is deferred, not in v1.
- No intercom through HA (external Asterisk only).
- No full media browse (MSP-style) — `media_player` primitives only.
- No HA-hosted licensing — licensing stays in the device + nuvoxel backend.
