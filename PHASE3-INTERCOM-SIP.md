# Phase 3 — Native Control4 Intercom / Announcements via SIP

Goal: play Control4 announcements (and full two-way intercom) **out of the
keypad's speaker** by joining Control4's native intercom as a standards-based
**SIP endpoint**, rather than the parallel custom `announce` message (which
stays as the lightweight chime+banner path — see [PROTOCOL.md](PROTOCOL.md)).

**Status: the driver side has shipped.** `driver-keypad/` builds
`NuVoxelKeypad.c4z` as a single multi-proxy driver with the `intercomproxy` (binding
5003) folded in alongside the primary `keypad` proxy (binding 5002) — see
`driver-keypad/README.md` for the proxy table and `intercom.lua` for the
implementation. This doc is now **as-built (driver) + remaining firmware
work** for 3b/3c: phases 3a (registration) and the driver half of 3b are done;
the firmware SIP/RTP stack (§4) and the two-way call path (3c) are still
ahead. It's grounded in the DriverWorks **Intercom proxy** spec and
Espressif's **esp_rtc** (esp_media_protocols / ESP-ADF VoIP) SIP stack.

---

## 1. How Control4 intercom actually works

- Every intercom endpoint (T3 touch screen, door station, …) is a **SIP user
  agent** with an **Address-of-Record (AOR)**: `sip-user@<director-ip>`.
- **Director is the SIP registrar** (the Communication/Intercom service). For
  non-Control4 SIP servers the creds are entered manually; with Director the
  driver provisions them.
- A call is a **SIP INVITE + RTP**. An **announcement / "monitor" call is just a
  one-way (`recvonly`) MONITOR-type call** (call type `2`). An endpoint with
  **auto-answer** accepts it and plays the incoming RTP → announcement out the
  speaker. That is exactly our target.
- Third-party SIP devices (DoorBird, 2N, Hikvision) join this way: the device is
  the SIP UA; a DriverWorks **Intercom proxy** driver bridges Control4's call
  model to/from the device and provisions SIP credentials.

## 2. Architecture (who does what)

The intercom is the **`intercomproxy` sub-proxy** (binding 5003) of the single
multi-proxy `NuVoxelKeypad.c4z` driver, alongside the primary `keypad` proxy (binding
5002) — see `driver-keypad/README.md`. There is no separate intercom driver
and no dedicated intercom port: everything rides the one `:6700` link the
keypad driver already owns.

```
   Control4 Director / other endpoints (T3, door stations)
     │  SIP REGISTER / INVITE / RTP (audio)              ▲ SIP / RTP
     ▼                                                   │
   ┌────────────────────────────┐                        │
   │ NuVoxelKeypad.c4z           │                        │
   │  • keypad proxy (5002)      │──TCP :6700────────────►│   ┌──────────────────┐
   │    now-playing, buttons     │  state/cmd + sip/call/  ├──►│ MMKeypad firmware │
   │  • intercomproxy (5003)     │  sipstate/callstate     │   │ (ESP32-S3)        │
   │    SIP creds = props,       │  (PROTOCOL.md)          │   │ • net.c  :6700    │
   │    bridges proxy⇄device     │                         │   │ • esp_rtc SIP UA  │
   └────────────────────────────┘                          │   │ • RTP ↔ ES8311    │
                                                             │   │ • call UI (LVGL)  │
                                                             │   └──────────────────┘
```

- **The DEVICE runs the SIP UA** (registers to Director, sends/receives INVITE,
  streams RTP through the ES8311). The Lua driver cannot do RTP, so all media +
  SIP signaling lives in firmware. One physical unit, one TCP link.
- **The `intercomproxy` sub-proxy** owns the **Intercom proxy** contract: it
  pushes the SIP credentials (from its own properties) to the device, relays
  proxy **Commands** (START_CALL/ACCEPT_CALL/END_CALL/…) down, and emits proxy
  **Notifications** (INCOMING_CALL/CALL_ACCEPTED/CALL_ENDED/…) up from
  device-reported state — making the keypad a first-class intercom endpoint in
  Navigator and programming.

## 3. Driver side (Control4) — `intercomproxy` sub-proxy of `NuVoxelKeypad.c4z`

The intercom lives inside `driver-keypad/` as a second proxy declared on the
same multi-proxy driver:
```xml
<proxy proxybindingid="5003">intercomproxy</proxy>
```
(the primary is `<proxy proxybindingid="5002">keypad</proxy>`; see
`driver-keypad/README.md` for the full proxy table). It shares the driver's
existing network binding to the device — no separate connection, no
intercom-specific port. This is why the driver ships under the new filename
`NuVoxelKeypad.c4z` rather than as an update to the old predecessor: Control4
only instantiates a driver's proxies when it's first added to a project, so
folding a new proxy into an *already-installed* driver would corrupt existing
projects. A fresh filename sidesteps that — every install of
`NuVoxelKeypad.c4z` gets both proxies from day one, and updates to *this*
driver still must never add/remove/retype a proxy per CLAUDE.md.

### 3.1 SIP configuration = driver properties (manual)
The `intercomproxy` sub-proxy exposes editable properties (no dependency on
Director auto-provisioning):
- **SIP Server / Domain** (default = Director IP; editable for an external PBX)
- **SIP Transport** (UDP / TCP / TLS), **SIP Port** (default 5060)
- **SIP Username / Extension**, **SIP Password**
- **Auto-Answer Announcements** (Yes/No), **Ringer/Speaker Volume**, **DND**

On connect (and on property change) the driver pushes these to the device as a
`sip` message; the device (re)registers. This mirrors DoorBird's manual
"SIP Domain/Username/Password" mode and unblocks 3a without reverse-engineering
Director's SIP account creation.

### 3.2 Capabilities (XML, declared in the proxy)
| capability | value | why |
|---|---|---|
| `driver_arch_type` | `5` | required for third-party drivers |
| `is_doorstation` | `False` | we're a station/panel, not a door |
| `has_intercom` | `True` | audio intercom endpoint |
| `has_video_intercom` | `False` | audio-only v1 |
| `use_speaker` | `True` | plays call/announcement audio |
| `use_microphone` | `True` | two-way (phase 3c) |
| `use_ringer` | `True` | rings on incoming |
| `has_auto_answer` | `True` | **required for announcements** |
| `has_do_not_disturb` | `True` | DND |
| `has_camera` / `has_display` | `False` / `True` | |
| `number_ring_buttons` | `0` | no call buttons (not a door station) |

### 3.3 Command / notification bridge
Proxy **Commands** (proxy → driver) we must handle → relay to device as `call`:
- `START_CALL(device_id, remote_device_id, audio_dir, video_dir, ring)` — outgoing
- `ACCEPT_CALL(device_id, remote_device, session_id, audio, video)`
- `END_CALL(device_id, remote_device, session_id)` → device SIP BYE
- `REJECT_CALL`, `MUTE_CALL`, `PAUSE_CALL`, `RESUME_CALL`, `START_MONITOR_CALL`
- State commands: `SET_RINGER_VOLUME`, `SET_SPEAKER_VOLUME`, `SET_DND`,
  `SET_AUTO_ANSWER`, `SET_MICROPHONE_GAIN`, `SET_MONITOR_MODE`, …

Proxy **Notifications** (driver → proxy) we emit from device-reported state:
- `INCOMING_CALL(device_id, session_id, call_type, remote_device_id, audio, video)`
  — call_type `0`=regular, `2`=monitor/announce, `3`=forking, `5`=external
- `OUTGOING_CALL(...)`, `CALL_ACCEPTED(...)`, `CALL_ENDED(...)`, `CALL_REJECTED`,
  `CALL_PAUSED/RESUMED`, plus state-changed notifications (volumes, DND,
  `SIP_USERNAME_CHANGED`, …).

`remoteAor` (the other party's `sip-user@ip`) threads through INCOMING_CALL →
ACCEPT_CALL → CALL_ACCEPTED so Control4 can correlate the two ends.

### 3.4 SIP credentials — manual (decided)
We provision SIP creds **manually via the driver properties** in §3.1 (Server,
Transport, Port, Username, Password). The dealer creates a SIP account on Director
(or any PBX) and enters it; the driver pushes it to the device. This is the
DoorBird "manual SIP" mode and is the committed approach — no dependency on
Director auto-provisioning.

*Future (optional):* auto-discover the endpoint's SIP account from Director so the
dealer doesn't hand-enter it. The exact `C4:` / proxy mechanism isn't in the
public docs (would come from the DriverWorks door-station template driver or a
live Director). Tracked as a nice-to-have; not required for any phase below.

## 4. Firmware side (ESP32-S3)

### 4.1 SIP stack: `esp_rtc` (esp_media_protocols)
- Add `espressif/esp_media_protocols` to `idf_component.yml` (already noted there).
- Core API (ESPRESSIF-MIT, ~250 KB / 111 KB IRAM — fits 16 MB/8 MB board):
  - start with a **SIP URI**: `"udp://<user>:<pass>@<director-ip>:5060"`
    (transport ∈ udp/tcp/tls) → auto-REGISTER.
  - `esp_rtc_call(h, "<ext>")`, `esp_rtc_answer(h)`, `esp_rtc_bye(h)`.
  - event callback for register/invite/ringing/answered/bye → drives our state +
    the TCP `call` notifications to the driver.
- Audio in/out: esp_rtc consumes/produces PCM. Bridge it to the **ES8311 via
  esp_codec_dev** (the `audio.c` we already built). On ESP-ADF this is the
  `av_stream` HAL; on plain IDF we provide read/write callbacks pulling from
  `audio_read` / pushing to `audio_write`. Confirm the exact esp_rtc audio
  callback/struct from the component's `esp_rtc.h` when scaffolding.

### 4.2 Media / codec
- Control4 intercom is typically **G.711 (PCMU/PCMA) @ 8 kHz**. Our codec runs
  16 kHz today → for calls, open the ES8311 at **8 kHz** (or resample). esp_rtc
  negotiates the codec in SDP.
- **AEC** (echo cancellation): there is **no hardware AEC** — the ESP32-S3 has no
  AEC peripheral and the ES8311 is a plain codec. AEC is **`esp-sr` software** (AFE
  algorithm, accelerated by the S3 vector ISA + PSRAM), fed the mic plus a
  **digital playback reference** (the PCM from `receive_audio`), run in the mic
  path before `send_audio`. Needed only for two-way (3c) where speaker+mic are
  live together and close-coupled; announcement-only (3b) is one-way → no AEC.

### 4.2a esp_rtc audio bridge (resolved — no ADF)
`esp_rtc` takes raw-PCM callbacks (`esp_rtc_data_cb_t`), so we do **not** use
ESP-ADF/`av_stream`. Only `esp_media_protocols` (+ `media_lib_sal`) is added:
- `send_audio(buf,len)` → fill from mic via `audio.c` (`audio_read`) → esp_rtc
  G.711-encodes + RTP-sends. Return bytes (0 to skip).
- `receive_audio(buf,len)` → decoded remote PCM → `audio_write` → speaker. (A 6-byte
  `"DTMF-"`+id payload here is a DTMF event, not audio.)
- `event_handler` maps `ESP_RTC_EVENT_REGISTERED/INCOMING/CALL_ANSWERED/HANGUP/
  AUDIO_SESSION_BEGIN/END/ERROR` → our state + the `sipstate`/`callstate` messages.
- Config: `uri="udp://user:pass@server:port"`, `acodec_type=RTC_ACODEC_G711U`,
  `local_addr`=our IP, `register_interval`, `user_agent`.

### 4.3 Call state + UI
- New firmware module `sip.c` (peer to `audio.c`): owns esp_rtc, REGISTER, the
  call state machine, and maps SIP events ⇄ the TCP `call` protocol.
- Reuse `audio.c` for the amp (enable on call/answer, disable on end) and the
  ES8311 path; `audio_play_chime()` for the ring.
- LVGL call UI on `lv_layer_top()` (mirror `ui_announce`/`ui_identify`):
  incoming-call card with Answer/Decline; in-call indicator; auto-answer path for
  announcements shows a banner only.

## 5. Protocol additions (driver ⇄ device, PROTOCOL.md)

These are **multiplexed onto the same :6700 socket** as the now-playing/keypad
protocol — there is no second TCP channel. `intercom.lua` (proxy 5003) sends
and receives them over the driver's one network binding; on the device, `net.c`
routes them to `sip.c`. See PROTOCOL.md's "Intercom / SIP" section.

Driver → device:
- `sip` — provision/refresh registration:
  `{"t":"sip","server":"192.168.1.50","port":5060,"transport":"udp","user":"1012","pass":"…","aor":"1012@192.168.1.50"}`
  Device (re)starts esp_rtc with this URI.
- `call` — call control mapped from proxy commands:
  `{"t":"call","action":"start|accept|end|reject|mute|resume","session":"…","remoteAor":"…","audio":0,"video":3,"ring":1}`

Device → driver:
- `sipstate` — `{"t":"sipstate","registered":true,"user":"1012"}` → driver emits
  `SIP_USERNAME_CHANGED` / registration status.
- `callstate` — `{"t":"callstate","event":"incoming|outgoing|accepted|ended|rejected","session":"…","remoteAor":"…","callType":2,"audio":2}`
  → driver emits the matching proxy notification (INCOMING_CALL/CALL_ACCEPTED/…).

(Unknown `t` still ignored on both sides for forward-compat.)

## 6. Phased build (each independently testable on a live Director)

- **3a — Registration. Driver side shipped.** `intercomproxy` (binding 5003) +
  capabilities + SIP-config properties (§3.1) are live in `NuVoxelKeypad.c4z`,
  pushing `sip` over the existing :6700 link. Firmware: `sip.c` needs to bring
  up esp_rtc from the creds and REGISTER, reporting `sipstate`. Validate the
  esp_rtc↔ES8311 audio bridge here. **Milestone:** device shows as a
  **registered intercom endpoint** in Composer (SIP Information populated;
  appears in the device list).
- **3b — Receive / announce (the goal).** Firmware: handle inbound INVITE →
  auto-answer MONITOR/announce calls → play incoming RTP out the speaker; ring +
  on-screen banner. Driver: emit INCOMING_CALL(type=monitor)+CALL_ACCEPTED.
  **Milestone:** a Control4 **announcement plays out the keypad speaker natively**.
- **3c — Two-way talk.** Firmware: mic → RTP send; AEC; answer/decline UI. Driver:
  full START/ACCEPT/END bridge. **Milestone:** intercom call to/from a T3 works.
- **3d — Polish.** Ringer/DND/volume state sync, monitor mode, optional door-chime,
  TLS/SRTP if Director requires, codec/AEC tuning, call-group support.

## 7. Open items / risks
- **esp_rtc audio bridge** on plain ESP-IDF (vs ADF `av_stream`) — verify the
  audio callback interface in `esp_rtc.h`; may need a thin shim to `audio.c`.
  *(Biggest unknown — validate first in 3a.)*
- **8 kHz codec reconfigure** alongside the 16 kHz UI/announce path (open/close
  the codec per call, or run dual-rate).
- **AEC quality** with the close-coupled mono speaker+mic (3c).
- **CPU/RAM** with WiFi + LVGL + SIP/RTP + AEC concurrently (budget looks OK:
  esp_rtc ~250 KB, board has 8 MB PSRAM, but validate real-time audio jitter).
- *Resolved by design:* SIP creds → manual driver properties (§3.4); proxy-add
  update-safety → shipping under the new `NuVoxelKeypad.c4z` filename means
  every install already has both proxies, so no existing project is ever
  mutated (§3).

## 8. References
- DriverWorks Intercom proxy: <https://snap-one.github.io/docs-driverworks-proxyprotocol/#intercom-proxy>
  (commands/notifications/capabilities pulled from `snap-one/docs-driverworks-proxyprotocol`).
- Composer Intercom proxy config: <https://docs.control4.com/help/c4/software/cpro/dealer-composer-help/content/composerpro_userguide/configure_the_intercom_proxy.htm>
- ESP-ADF VoIP example / `esp_rtc`: `espressif/esp-adf` `examples/protocols/voip`
  (`sip_service.h`, `voip_app.c`); component `espressif/esp_media_protocols`.
- Reference third-party SIP integrations: DoorBird, 2N, Hikvision Control4 drivers.
