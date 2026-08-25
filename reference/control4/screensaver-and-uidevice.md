# Control4 native screensaver / wallpaper / UI-device plumbing — research

Reverse-engineered against the dev EA-3 Director (`192.168.1.220`, OS 4.2.1) and
the extracted T3 Navigator APK. Two questions drove this:

1. Can we feed **cloud photos** (iCloud/Google) into Control4's native
   screensaver/wallpaper from a driver? → **No, not from a sandboxed `.c4z`.**
   The native path (§1) needs a media-storage share; pure-`.c4z` gets only a
   WebView experience takeover.
2. Can our **keypad** consume the native screensaver/brightness command plumbing
   by presenting as a `uidevice`? → **The command vocabulary (brightness AND the
   whole screensaver/wallpaper/DND set) is base-`uidevice`, so it's reachable IF
   Director dispatches to a third-party `uidevice` (Gate B). Only the Composer
   *settings UI* is first-party-gated. Gate B is very likely YES — the spike's
   settings UI rendered on double-click — final `ReceivedFromProxy` confirmation
   pending (§3).**

See also memory notes `director-media-photo-path` and `keypad-as-uidevice`.
Method note: everything below was recovered read-only from the dev EA-3 via SSH
(see our internal Director-access notes (not published)) plus the decompiled
`phoenix-navigator.apk` and `strings`/`objdump` on the native `.c4l` driver ELFs.

---

## 1. How the native photo screensaver actually works

- **Photos are NOT scanned into a DB.** `/mnt/internal/db/mm.db` indexes music &
  movies (song/album/movie/FTS) — **no photo table**. So there is no "scan photos
  into the media DB" step to hook.
- **Photos are a URL push, not a file scan.** From the T3 Navigator
  (`phoenix-navigator.apk`, native `com.control4.phoenix.screensaver.
  ScreensaverManager`, an Android **DreamService**): the director broadcasts
  `SYSTEM_DATA_SCREENSAVER_PHOTO` carrying `SYSTEM_DATA_EXTRA_PHOTO_URL`
  (an `http://director/...` media-library URL) + `SYSTEM_DATA_EXTRA_PHOTO_TIMEOUT`.
  The panel downloads + caches (`getPhotoUrl`, `getPhotoCacheDir`, `DIR_PHOTOS`)
  and rotates. Same `SystemData` channel feeds weather (`*_TEMP`) and Current-Media
  (`ALBUM`/`ARTIST`) screensaver modes. **The panel will display any http URL it is
  handed** — but only the privileged director screensaver agent emits that push,
  and it sources URLs from the media library.
- **Media library = web-served directory**, not a driver surface. nginx serves
  `/mnt/internal/www/`; photos come from the conventional symlink
  `www/control4_pictures` → a media-storage share (an iMac SMB folder in this
  project; currently a dangling symlink), plus `www/control4_apps` (USB).
  Controllers run **Samba** (`smbd`/`nmbd` on :445/:139) exporting `/media` to each
  other — that's the `//core1-.../media` CIFS mounts under `/mnt/internal/net/`.
- `imageservice.js` (`/opt/control4/tr3/`, node + `sharp`/libvips, has `axios`) is
  the resize/thumbnail/cache layer, not an enumerator.
- **No FUSE** on the controller (`/dev/fuse` absent, no `fuse.ko`) → no lazy
  virtual-filesystem mount without an out-of-tree kernel module.
- Screensaver modes are **hardcoded in the panel firmware**
  (`none|blank|dateandtime|media|photos|custom`); **`custom` launches an Android
  package** (`SCREENSAVER_PACKAGE_NAME`), NOT a settable URL.

### Verdict for cloud photos
| Tier | Native screensaver? | Pure `.c4z`? | What it needs |
|---|---|---|---|
| 1 | ✅ | ❌ impossible | blocked by platform (privileged push, no library write, custom=package) |
| 2 | ✅ genuinely native | ❌ needs infra | Composer **media-storage share** → a companion Linux box running a lazy iCloud→SMB proxy (Samba + FUSE/VFS, fetch-on-read, no mirror). `.c4z` shrinks to orchestration (set mode/delay, trigger rescans). **Only route to the real native screensaver.** A **re-purposed T3 (RK3188 Linux)** is a good host; an **ESP32 keypad is not** (no SMB stack, can't cache multi-MB JPEGs). |
| 3 | ❌ (experience takeover) | ✅ self-contained | **WebView experience** (`uibutton` + `web_view_url`, template `control4_webview_sample.c4z`) + `C4:url()`/`CreateServer` slideshow + `SCREENSAVER_ACTIVATE`/mode commands to take over the idle screen. |

**Note:** panels running *our* T3 firmware don't need any of this — render the
iCloud slideshow directly in-firmware. The native-path work is only for STOCK
Control4 panels we don't control.

**Idea evaluated & rejected — "a `.c4z` that hosts a fake SMB share proxying
iCloud" (so no bulk mirror):** architecturally right (lazy proxy, not a copy) but
NOT hostable from a driver: (a) the controller **already runs `smbd` on :445** —
the port is taken, a c4z can't bind it; (b) `C4:CreateServer(port,"TCP")` is a bare
TCP listener whose existence a driver even hedges on — hand-rolling SMB2/3+NTLMSSP
in sandboxed Lua is impractical; (c) even a working share isn't consumed without a
`mount.cifs` + media-storage config (root/Composer), and the sandbox can't write the
`/media` dirs smbd serves. ⇒ the proxy share must run on a **companion host**
(Tier 2). A re-purposed **T3 (RK3188 Linux)** can host it; an **ESP32 keypad
cannot** (no SMB stack; 16 MB flash / tens-of-MB PSRAM can't cache multi-MB JPEGs;
it's busy). Panels on our own firmware just render photos directly and skip all of
this.

---

## 2. Keypad-as-`uidevice` (consume native brightness/screensaver commands)

`uidevice` is the proxy real touchscreens present. Its command vocabulary (from
`strings` of the encrypted `/control4/drivers/uidevice.c4l`) maps ~1:1 onto a
keypad: `SET_BRIGHTNESS`, the whole `SCREENSAVER_*` set, `SET_WALLPAPER`,
`SET_DARK_MODE`, `DND`, `PLAY_ANNOUNCEMENT`, `PHYSICAL_BUTTON_*`.

### Gate A — refined by the native driver binaries (see §4)
The `SCREENSAVER_*`/`WALLPAPER`/`DARK_MODE` commands are **base-`uidevice` native
commands** (the compiled `uidevice.c4l` class `uidevice` handles them for *any* UI
device), NOT glassedge-specific. The first-party gating is ONLY in the Composer
**programming menu** (broker `uidevice_glassedge.js` surfaces those programming
actions for glass; generic `uidevice.js` surfaces only BRIGHTNESS/popups). So the
command *capability* is universal to the `uidevice` proxy; what's glass-gated is
the Composer settings/programming UI, not the protocol.

### Gate A (original observation) — programming surface (broker)
`/mnt/internal/node/broker/programming/`:
- **Generic `uidevice.js`** — `getCommands` has **NO manufacturer/first-party
  gate**; it offers `BRIGHTNESS`, `HIDE_POPUP`, `SHOW_POPUP`, `SET_STATUS` to any
  device with a valid item. Conditionals key off the driver's **declared
  capabilities** (`has_backlight`, `has_auto_brightness`, sensor list from
  `GET_SETUP`/`GET_STATE`), not identity. `has_backlight` defaults true.
- **`uidevice_glassedge.js`** — the FULL set (`SET_SCREENSAVER_MODE` [modes
  none/blank/dateandtime/media/photos/custom], `SET_SCREENSAVER_START_TIME`
  [15s–1h], `SCREENSAVER_ENTER/EXIT`, `SET_AUTOBRIGHTNESS_ENABLED`,
  `SET_PROXIMITY_SENSOR_ENABLED`, `SET_ALEXA_ENABLED`) is here, gated on the glass
  device type (`c4:uidevice_glassedge10_touchscreen`, per `glassedge10.conf` →
  Control4's first-party `uidevice_glassedge10.c4i`).
- ⇒ Even if dispatch works, a NuVoxel keypad gets at most **BRIGHTNESS + popups +
  status**, NOT the screensaver/wallpaper set (can't claim the glass identity).

### Gate B — runtime dispatch (does Director drive a third-party `uidevice`?)
- Spike `NuVoxelUidevSpike` (still on Director at `/mnt/internal/c4z/`) tests this.
  v2 used `<combo>true</combo>` → PROXY item with no child proxy, `ReceivedFromProxy`
  never fired. Commit `6d9a836` recorded the proven finding: **`<combo>` suppresses
  proxy instantiation** (the v2 red herring). Whether v3 (combo removed, proper
  `<proxy proxybindingid=5001>uidevice</proxy>`) is actually DRIVEN was left open;
  standing hypothesis: "uidevice reserved for first-party Navigators."
- **STATUS: being settled now** — see §3.

### Pragmatic recommendation
Don't ship a `uidevice` proxy (payoff caps at brightness/popups; proxy sets freeze
at add-time; risk Director ignores it). Keep the working `keypad_proxy`(5001) +
`intercomproxy`(5003). Firmware already does brightness + idle screensaver-dim
(`screensaver_sec`, backlight PWM, dealer-pushed via `net.c`). Do system
integration by exposing our OWN commands + reacting to scene/variable/adv-lighting
events.

---

## 3. Settling Gate B — live spike run

**Instrument (verified working).** `NuVoxelUidevSpike/driver.lua` logs every entry
point (OnDriverInit/LateInit, `ReceivedFromProxy`, OnBindingChanged,
OnConnectionStatusChanged, GetSetup/GetState) and phones home via `C4:url():Get()`
to `http://192.168.46.146:8899/` (the dev Mac — IP still current). Director→sink
path confirmed: a director-side `curl` hit the sink (`http_code=200`, logged).

**Decisive signal:** if `ReceivedFromProxy` fires when a `BRIGHTNESS` command is
sent → Director drives a third-party `uidevice` (brightness path viable). If it
never fires → `uidevice` is Navigator-only; drop it.

**Strong prior evidence Gate B is YES:** when the spike was instantiated in a
previous session, **double-clicking the instance brought up the Composer UI-device
settings page** — i.e. Director accepted a third-party `uidevice` proxy and rendered
its native settings. Combined with (a) no manufacturer gate in `uidevice.js` and
(b) the vocabulary being base-`uidevice` (§5), the "reserved for first-party" fear
looks false. The one thing not yet *directly* logged is `ReceivedFromProxy` firing.

**Why not added programmatically (investigated this session):**
- `dman project` (`/control4/bin/dman`) has **no single-device add** — only
  `clear|disable|enable|export|import|list|remove`. `import` is a full-project
  replace (too blunt); `export --file` is a safe read-only backup; `remove`/`disable`
  are the safe cleanup verbs.
- SOAP `AddDevice` exists — entry tag
  `<c4soap name="AddDevice" session="0" operation="RWX" category="composer" async="False">`
  on :5020 — but its payload is a full device-XML blob (config_data_file, parent,
  proxy bindings, images) built inside **webpack-obfuscated** broker code; the REST
  route that would build it correctly is obfuscated too (string-array-indexed paths).
  A hand-built insert risks a malformed item / Director instability (history: an
  unrelated spike crashed the Director on removal). Not worth it vs. a 30-sec
  Composer add.

**Definitive step remaining:** re-add **“NuVoxel uidevice Spike”** in Composer
(Playroom); sink (Mac :8899) is live and the director→sink path is proven
(`http_code=200`). On bind, fire `director.sh execute <id> BRIGHTNESS:50` and read
whether `ReceivedFromProxy` logged to the sink. Then `dman project remove <id>` or
remove in Composer.

**RESULT (settled 2026-07-21, spike instantiated as device 3369 in Office):**
- ✅ Director **instantiates** the third-party `uidevice` proxy — `OnDriverLateInit`
  fired, `GetProxyDevices()` = 3369 (scalar).
- ✅ Composer **renders the native UI-device settings + generic actions** for it —
  a Popups panel (SHOW_POPUP: message/IMGURL/delay/showok) + Status (SET_STATUS).
  So the "reserved for first-party" fear is **false** at the project/UI level.
- ❌ **NO command ever reached the driver.** Tested three independent paths, spike
  logging `ReceivedFromProxy`/`ExecuteCommand`/`UIRequest`/`GetSetup`/`GetState`
  all to the sink: (1) Composer Popups/Status buttons, (2) `SendToDevice` via
  `director.sh execute 3369 <cmd>`, (3) a **Programming** "Set Brightness → 50"
  action. Sink shows only the two `OnDriverLateInit` events — **nothing else**.
  Director never even called `GetSetup` on the driver.
- **Interpretation:** an inert Lua `uidevice` `.c4z` is instantiated but **not
  driven** — Director dispatches UI commands only after a navigator handshake the
  spike never performs (respond to the initial proxy `GET_SETUP` / report
  online/registered), OR only to native/first-party uidevice drivers
  (`uidevice.c4l`, C++). Can't distinguish the two from an inert probe, but the
  product implication is the same: **declaring `uidevice` in a Lua `.c4z` is NOT
  sufficient to receive brightness/screensaver commands.** Getting them would mean
  reverse-engineering + implementing the full navigator handshake on an
  undocumented, add-time-frozen proxy — not worth it.

**WHY (from the OEM driver `/opt/control4/var/drivers/c4i/uidevice_glassedge10.c4i`):**
a real touchscreen declares much more than the `uidevice` proxy —
- `uidevice` + `intercomproxy` proxies,
- a **`UI_DEVICE` connection** (class `UI_DEVICE`, type 2),
- a **`Touchscreen IP Connection`** — type-4 **network** connection, `TCP`+`SSL`,
  name **"Sysman"**, `keep_connection`/`monitor_connection` — the **live socket the
  panel hardware maintains to the Director**,
- capabilities `has_navigator=true`, `has_display=true`, `has_manual_brightness`,
  `has_suspend`, `has_do_not_disturb`, `has_intercom`, …

The UI commands are dispatched **to the connected navigator over that Sysman
TCP/SSL socket** (gated on `has_navigator` + an actually-connected/online panel),
NOT to a DriverWorks Lua proxy handler. Our spike declared only the `uidevice`
proxy — no `UI_DEVICE` connection, no navigator socket, nothing connected — so there
was no navigator to receive commands. **To consume the native uidevice plumbing a
device must connect to the Director *as a navigator* (speak the Sysman TCP/SSL
navigator protocol), which is a full Navigator-client implementation, not a
`.c4z`.** ESP32 keypad: impractical. T3-on-our-firmware: theoretically possible but
= reverse-engineering + reimplementing a Control4 Navigator. Out of scope.

**FINAL nail — the proxy c4i exposes NO consumable interface.** Base
`/opt/control4/var/drivers/c4i/uidevice.c4i` (955 bytes) has `<commands/>`,
`<events/>`, `<conditionals/>` **all empty** and `<proxy/>` empty; it declares only
a `UI_DEVICE` **connection** (`<id>5001</id>`, `type 2`, `<consumer>True</consumer>`,
class `UI_DEVICE`). So there is no c4i-level command protocol for a Lua driver to
hook. Every command (`SET_BRIGHTNESS`, `SET_SCREENSAVER_MODE`, `SHOW_POPUP`, …) is
baked into the compiled native `uidevice.c4l` (`OnCmdSetBrightness` C++ handlers) +
the navigator wire protocol — NOT exposed as a proxy interface. To consume them you
must be the native `.c4l` or a connected navigator on the `UI_DEVICE`/Sysman socket.
Not reachable from DriverWorks Lua.

**Decision: do NOT pursue `uidevice` for the keypad.** Keep `keypad_proxy`(5002) +
`intercomproxy`(5003); firmware already does brightness + idle screensaver-dim;
expose our own commands + react to scene/variable/adv-lighting events.

**Cleanup:** remove the spike (`dman project remove 3369` or Composer),
delete `~/Documents/Control4/Drivers/NuVoxelUidevSpike.c4z`, stop the Mac sink.

---

## 4. Glassedge internals worth noting (from the APK)

- Screensaver = Android **DreamService**; `SystemData` push carries screensaver
  content (photo URL, weather temps, now-playing) — see §1.
- Full command set observed: `SCREENSAVER_ACTIVATE(_EXTRA_ACTIVATE/_RESET_TIMER)`,
  `SCREENSAVER_MODE`, `SCREENSAVER_EXTRA_JSON_CONFIG`, `SCREENSAVER_PACKAGE_NAME`,
  `SCREENSAVER_SCREEN_OFF_ENABLED/START_HOUR/STOP_HOUR`, `SCREENSAVER_BRIGHTNESS`,
  `SET_BRIGHTNESS_TARGET`/`SET_BRIGHTNESS_STOP` (ramp), `SET_DND`,
  `SET_ANNOUNCEMENT_DND`, `screensaver_activate_on_dock`/`_on_sleep`.
- Panel↔web bridge (`assets/api/c4.js`): `sendCommand`, `subscribeToVariable`,
  `subscribeToDataToUi`, `onDataToUi`, `closeWebView` via `sendNativeMessage("c4")`
  — relevant to the WebView-experience path.
## 4b. The List Navigator — a driver-consumable browse menu (unlike `uidevice`)

The SR260 remote (and the on-screen list UI) use the **List Navigator**, and unlike
the touch `uidevice` (empty proxy, native-only, §3) it **is bound into DriverWorks
Lua** — a `.c4z` can build browse menus and receive selections. This is "a better
way to the browse menu."

- Engine: `lua_gen_jit.c4l` exposes `lua_gen::NewList(INavigatorItem,…)`,
  `NewControl(INavigatorItem,…)`, `HandleEvent(ListNavigatorEvent,…)`;
  `IListNavigatorClient`/`INavigatorItem`; core lib `/control4/lib/liblistnavigator.so`.
- **DriverWorks Lua callbacks** a driver implements (from the *unencrypted*
  `drivers-common-public/global/handlers.lua`, e.g. in `room_control_keypad` — a
  Control4 KEYPAD driver):
  - `ListNewList(nListID, nItemCount, strName, nIndex, strContainer, strCategory, strNavID)`
    — navigator asks the driver to populate a list
  - `ListNewControl(strContainer, strNavID, idDevice, tParams)` — build a control/screen
  - `ListEvent(strEvent, param1, param2)` — selection/navigation events to the driver
  - `ListMIBReceived(strCommand, nCount, tParams)` — list data block
- So a keypad `.c4z` can present Control4 browse menus + handle selection, in Lua —
  the readable `room_control_keypad` common-lib is the reference implementation.

### The List Navigator is the LEGACY remote system (verified) — NOT Neeo/Halo
`liblistnavigator.so` (`plugins/nav/list_navigator/Remote.cpp`) drives client
classes **`CLegacyRemote`**, **`CUnicodeRemote`**, **`CRemote`**, **`SR260FontInfo`**
(`NewList()`, `InterpretSelectMIB()`, `ItemSelectEnd()`, "Keypad Brightness"). So
its clients are the SR260 + older remotes + the old **LCD Keypad** (`kpz-10b1-w`,
c4i `control4_10button_keypad`, name literally "LCD Keypad" — only the manifest
survives on 4.2; driver code pruned). `room_control_keypad` (plaintext common lib)
is the readable reference for the `ListNewList`/`ListNewControl`/`ListEvent` API.
**Verified NEGATIVE for Neeo/Halo:** zero "neeo"/"halo" strings in
`liblistnavigator`, the director, or the engine; their `driver.xml` declares only
`control4_neeo_remote`/`NEEO_REMOTE` (and Halo `voiceinput`), no navigator/list
decls; they ship no `drivers-common`/`handlers.lua`. → **Neeo/Halo do NOT use the
list navigator** — they use their own custom protocol (modern navigator), UI fed
via the encrypted Lua. So the list navigator is a *legacy* (but still-present in
4.2) driver-consumable browse path; modern devices went custom.

### Neeo / Halo remotes (this project has both) — custom, not list-nav
- Neeo: `control4_neeo_remote.c4z` (Office Remote, dev id 1750), proxy
  `control4_neeo_remote`, connection class `NEEO_REMOTE`. Halo:
  `control4-halo-remote.c4z` (+ `-hub`, dev id 2697), proxy `voiceinput.c4i`
  (voice/mic), SIP registration. SR260: `control4_sr260.c4i` (dev 3062), Zigbee,
  `<control>control4_remote_gen</control>`.
- **The driver's `www/` is NOT the device UI** — it's the Composer-side **HTML
  Properties page** (Neeo `www/html/index.html` = a built React SPA, meta
  `"Control4 HTML Properties Page"`) + `Documentation.rtf` + localized
  `driverstrings/*.po` + icons. The device's on-screen UI is rendered by the
  device's OWN firmware (Neeo screen / our ESP+LVGL) from **list-navigator data**
  the driver supplies. (Distinct from the OS3 touch-panel case §1/§3, where
  `phoenix-navigator` on the PANEL web-renders — also not the driver's www, except
  the `web_view_url`/`controller://driver/...` WebView-experience path.)
- **Their glue Lua is ENCRYPTED** (`driver.lua.encrypted`, `<script encryption="2"
  jit="1">` → PKCS#7/AES-256+RSA to *DriverWorks Encryption V2*, LuaJIT bytecode —
  see our internal Director-access notes "Encrypted DriverWorks Lua"). But the **`www/` (touch UI)**
  + `driver.xml` (proxy/connection structure) + the **unencrypted list-navigator
  common lib** already show the hybrid pattern: custom proxy + web touch UI + list-nav
  callbacks. Decrypting the Neeo Lua itself is possible (RE `lua_gen_jit.c4l` for the
  key, or heap-scrape) but not needed to replicate the pattern.
- **Keypad takeaway:** for a browse menu, implement the List-Navigator callbacks
  (`ListNewList`/`ListNewControl`/`ListEvent`) in the keypad driver — a real,
  Lua-reachable path (unlike `uidevice`), demonstrated by `room_control_keypad`.

## 4c. The MODERN navigator (cerebellum tr2/tr3) — Neeo/Halo/panels, fully READABLE

This is how Neeo/Halo (and modern panels) actually render — and it needs **no
decryption** (the encrypted Neeo Lua is just driver-side glue; the UI is
server-generated). Found via the firmware/web-service angle.

> **Terminology — TR2 vs TR3** (Control4-internal, likely "Technology Release"; all
> at v1.80.2 here per `/opt/control4/tr3/versions.json`):
> - **TR2 = the navigator UI generation** — the declarative **Mustache-templated
>   XML** framework that gets *rendered* (`filerepo/tr2/` templates, `TR2_GUIXML`,
>   `TR2_GUIDATA`, `TR2_LIST`, `tr2:auth:hmac`, the tr2 HTTP API on `:3001`). It's
>   *what our keypad would draw*.
> - **TR3 = the services/runtime platform** — `/opt/control4/tr3/` = the Node
>   microservices **cerebellum** (generates/serves the TR2 GUI) + **imageservice**
>   (resizes images). It's *what our keypad would talk to*.
> - Relationship: **cerebellum (a TR3 service) generates TR2 GUI XML**; the
>   `neeo_release.bin` firmware is just a TR2 renderer. (So "cerebellum lives under
>   `tr3/` but its output is `filerepo/tr2/`" is expected, not a typo.)

- **Server:** `cerebellum` (`/opt/control4/tr3/cerebellum/cerebellum.js`) serves a
  navigator over endpoints **`/gui`, `/directory`, `/listLength`, `/tr2`**. The
  device is a **thin renderer**: fetch server-generated screens, render, POST events
  back. (Firmware image `neeo_release.bin` v1.80.2 + `neeo.xml` manifest live in
  `filerepo/tr2assets/`; the firmware is just the renderer.)
- **UI = Mustache-templated XML** in `/opt/control4/tr3/cerebellum/filerepo/tr2/`
  (copied to scratchpad `tr2-navigator/`, 54 files). Primitives: `button`,
  `textView`, `icon`, `header`, `image`, `screen`, `key`, **`listContent`/`listItem`/
  `listHeader`** (the browse list), `dots`. Data-bound via `{{…}}`.
- **Browse menu** = `/directory` + `list-dynamic.tpl.xml` (`<listContent listLength
  listOffset>` + swipe/back events) + `listitem.tpl.xml` (per-row `listItem` with
  text/icon/image/description/buttons). Server generates the list; device renders it.
- **Hard buttons** = `gui/keypad.tpl.xml` `<keypadMapping><key id="POWER"
  code="#0001">…` mapping keys→events.
- **Event/action model:** `onClick`(410)/`onPress`/`onRelease`/`onLongPress`/
  `onSlideToLeft|Right` → `ChangeScreen`, **`TriggerAction`**, **`RoomCommand`**,
  `ShowPopup`. e.g. `onRelease,TriggerAction,RoomCommand<sep>POWER<sep>press`.
- `gui/dynamicScreens` + `gui/dynamicProxies` (`dynamicFunction`/`dynamicTransport`)
  = dynamic/source content; `guidata/`, `startup.tpl.xml` (73 KB) = boot GUI +
  theme data; `filerepo/strings/*.json` = localization.

### Browse-menu paths — final comparison
| | Legacy **List Navigator** | Modern **cerebellum tr2/tr3** |
|---|---|---|
| Users | SR260, `CLegacyRemote`/`CUnicodeRemote`, old LCD Keypad | Neeo, Halo, modern panels |
| Where the UI is defined | driver Lua (`ListNewList`/`ListEvent` callbacks) | **server templates** (readable XML, cerebellum) |
| Device role | remote is a list client | **thin renderer** of server XML, posts events |
| Readable without decryption? | yes (common-lib callbacks) but legacy | **yes — the whole template system is on the box** |
| Fit for NuVoxel keypad | legacy, driver-side | **be a tr2/tr3 renderer**: GET `/gui`+`/directory`, render `listContent`/`listItem`/`button`/`textView`, POST `ChangeScreen`/`RoomCommand`; keypad hard-buttons via `keypadMapping`. Modern, and fully spec'd by the copied templates. |

**Net:** decrypting the Neeo/Halo Lua is unnecessary for the browse-menu goal — the
modern navigator UI is a **readable, server-driven templated-XML system** we can
render directly. (Decryption still doable via director-memory carve — root CAN read
`/proc/<pid>/mem`, key is resident — or RE of `lua_gen_jit.c4l`; but low value now.)

## 4d. Navigator (tr2 "remote") pairing / auth — the dealer-less-install angle

**Strategic goal:** get our device onto a **non-rooted, production controller
WITHOUT a dealer** by riding Control4's **existing QR + C4-app pairing flow** (the
homeowner path) instead of reimplementing pairing. Research; not built.

**How real remotes pair (OBSERVED, 2026-07-21 — corrects earlier QR assumption):**
- **Neeo = direct on-device LAN self-registration** (NO QR, NO app, NO cloud): the
  remote **discovers directors on the LAN** → user **picks the director** → user
  **picks the room** → registered. This is the cleanest dealer-less/cloud-less flow.
  Confirmed live: re-pairing produced `MANAGEMENT_DRIVER_CONNECTED { remoteId:
  '<remote-id>' }` — and the remoteId was **stable across delete+re-pair** (it's
  a device ID, and *is* the binding key / `Remote ID` in the driver).
- **Halo = QR + C4 app** (the earlier "scan a QR" description) — a different, newer,
  account/cloud-authed path via the Halo Remote Hub agent's JWT.
- ⇒ For our keypad, **replicate the Neeo flow**: SDDP-discover the director, present
  director+room selection, self-register, receive the per-remote key, sign tr2
  requests. No dealer, no Composer, no cloud.
  **Clean re-pair captured (2026-07-21, system healthy):** delete Neeo item 3390 →
  re-pair → new item **3391 "Office Neeo Remote"** (`control4_neeo_remote.c4z`, same
  device-stable `remoteId <remote-id>`, in the Office room). Confirms the
  direct-LAN self-register end to end. **But the key mechanism is hidden:**
  `store.json` stayed absent and **no key/store file was created** → the per-remote
  HMAC key is NOT persisted to a flat file; it's in the Remote Manager's **encrypted
  driver state** (`control4_remote_hub/driver.lua.encrypted`, 54 KB, PKCS#7 /
  DriverWorks-Encryption-V2) or **session memory** (likely re-negotiated per
  connection). The remote-hub Lua doesn't log to `director.log`, so the
  `AddRemote`/key-bootstrap is opaque from logs. **To map the crypto:** (a)
  packet-capture Neeo↔director (`:3001` HMAC + `:3004` manager comm) live, or (b)
  decrypt `control4_remote_hub`'s Lua (the our internal Director-access notes RE task).

### SOLVED — full auth + protocol (director memory-scrape, 2026-07-21)
Scraped the running director's memory (`node` → `/proc/<pid>/mem`, see
our internal Director-access notes "memory scrape") and captured the **live remote↔director protocol** +
the auth token. Cleaned capture: [`tr2-protocol-capture.txt`](tr2-remote/protocol-capture.txt).

- **Auth = a director-MINTED JWT pushed to the remote via the `Token` command**
  (NOT the HMAC key we hypothesized — HMAC is a separate request-signing layer). The
  JWT (RS256, signed by the director's own key) carries:
  `{UserName:"system.control4_ea3_<mac>@control4.com", CommonName:"control4_ea3_<mac>",
  Permissions:"/director,/auth/user,/concierge,/remote,/lightning",
  Realm:"remote.control4.com:5080", SystemTime, iat, exp (~30-min life)}`. The
  director mints it **locally with its own signing key** — that's why `/api/v1/localjwt`
  (which only gives `director,sysman`) was rejected earlier; the remote's token is a
  richer system-user credential. The Halo hub's **"Set JWT Token"** debug action
  injects exactly this. **⇒ Fully local + cloud-less: the director hands our device
  the token on registration; nothing to mint or steal.**
- **Protocol = newline JSON**, shape `{command|reply, type:get|invoke|notify|set|reply,
  messageId, remoteId, status, ...payload}` over the Remote Manager channel (`:3004`).
  Vocabulary captured: `RemoteInfo` (announce ip/mac/appVersion/bootloader), **`Token`**
  (auth push), **`GetItems{listId,firstItem,numItems}`** → **`NewList{category,count,
  label,listId,backAction}`** (browse — `List.ListenWatchLISTEN`, `List.Static`),
  `RoomInfo`, `ActiveMediaInfo`, `VolumeInfo`, `BrightnessInfo`, `BatteryLevel`,
  `WifiStatus`, `TactileButtonNames`, `LockToRooms`, `ConfigSleep`, `LogConfig`,
  `KeepAlive`/`Ping`, `AddRemote`.

**Complete dealer-less flow (now fully mapped, no unknowns):** device SDDP-discovers
director → user picks director + room → `AddRemote` → **director mints & pushes the
system-user JWT via `Token`** → device auths (Bearer + HMAC) → `GetItems`/`NewList`
browse → render tr2 XML. No dealer, no Composer, no cloud. This JSON message set +
the §4c tr2 templates = the full spec our S3 firmware would implement.

**Deobfuscation method (reproducible):** cerebellum.js hides identifiers as
**base64 entries in a string array**; `python -c base64.b64decode` over all
`[A-Za-z0-9+/]{8,}={0,2}` tokens yields ~1956 cleartext strings. Artifacts in
scratchpad: `cerebellum.js` + the decoder; the tr2 template set in `tr2-navigator/`.

### Transport & API
- HTTP REST: nginx **`0.0.0.0:3001`** → cerebellum **`:3002`** (CORS; plain HTTP, no
  mTLS). Static firmware/assets under `/tr2assets/` (`neeo_release.bin`, `neeo.xml`).
- Per-remote endpoints: `/:remote_id/gui_xml`, `/guidata_xml`,
  `/:remote_id/directory/browse`, `/directory/listback`, `/:remote_id/setup/:room_id`,
  `/:remote_id/key`, `/registeredRemotes`. `KEYPAD` is a first-class remote type.

### Auth = HMAC request-signing (not bearer tokens)
- Namespace **`tr2:auth:hmac`**. Device `getNonce` → signs payload with its
  **per-remote secret key** (`createHmac`+`sha256`) → `Authorization` header →
  server `verifyNonce`+`verifyHmac` (`SIGNATURE_MISMATCH`/`signatureValid`,
  `HTTP_INVALID_NONCE_OR_SIGNATURE`). Same scheme on a **UDP** channel.
- All endpoints `401` without a valid signature; the director JWT is rejected;
  `localjwt` only mints `director,sysman` (ignores requested `Services`).
  **localhost does NOT bypass** (tested 127.0.0.1:3001/3002 → 401).

### Key provisioning — director-MINTED at pairing, NOT factory-baked
- cerebellum `randomBytes`-generates the per-remote secret at pairing and delivers it
  to the device (wrapped to the device's `getPublicKey`) via `/:remote_id/key`;
  persists `{remoteId → secret}` to **`store.json`** (cerebellum CWD
  `/opt/control4/tr3/cerebellum`, created lazily on first registration — none present
  on the dev box = nothing currently paired via cerebellum).
- ⇒ You can't invent a key it accepts for an existing remote (validated vs the store),
  but **pairing hands you a fresh valid key**. (`remote_keyd` is unrelated — it maps
  button presses → uinput `c4.os.*` keys.)

### Add flow — cerebellum adds the device itself (no dealer at the add step)
- cerebellum `installAndConnectToRemoteManagerDriver`; on `AddRemote(remoteId,
  roomId)` (`COMMAND_ADDREMOTE`) it runs `<c4soap name="AddDevice">` **itself** (+
  `UpdateProjectC4i`/`GetItems`/`IsProjectLocked`), registers it to a room
  (`REGISTER_REMOTE_TO_ROOM`), and proxies remote↔driver via a `messageProxy`.
- **Remote Manager driver = `control4_remote_hub.c4z`** (proxy `control4_remote_hub`,
  connection class `NEEO_REMOTE`, `<control>lua_gen`; the Halo variant is
  `control4-halo-remote-hub.c4z`). It's required (`NO_REMOTE_MANAGER_DRIVER_FOUND`,
  `REMOTE_MANAGER_LOCKED`). The added remote's driver is `remoteDriverFileName`
  (e.g. `control4_neeo_remote.c4z`); cerebellum can handle the driver files
  (`C4Z_PATH`, `communication:lifecycle:driverFiles`, `/tmp/hid-c4z/`).

### Strategy: ride the existing flow (device generates QR → C4 app scans → added)
Best play (per design discussion): don't reimplement pairing. Our device generates a
valid QR; homeowner scans with the C4 app; cerebellum does the privileged
`AddRemote`/key-mint. This is dealer-less and root-less by construction. A paired
remote/hub driver of ours could then bootstrap the rest (our agent self-installs the
keypad drivers today).

**The two GATES that decide if this works (need testing — no impl yet):**
1. **Device-identity acceptance:** does the C4 app pair a *non-Control4* device, or
   only recognized products? No signed-identity/cert/allowlist gate *surfaced* in the
   cerebellum strings, but the app side is unknown — the QR is generated on the
   remote (Neeo firmware / driver), and the app may only accept Control4 product
   types. Likely we'd either masquerade as a `NEEO_REMOTE`-class device or need our
   own remote+manager driver pair recognized.
2. **Whose driver gets installed:** `AddRemote` installs `remoteDriverFileName`. If
   that's driven by the device's announced type and cerebellum will fetch/install
   OUR c4z, we get our footprint; if it only maps to known Control4 remote drivers,
   we'd land as a "fake Neeo" running Control4's driver, not ours.

**Next steps (when pursued):** (a) capture a real Neeo/Halo QR + the app→director
pairing calls to see the payload + any identity check; (b) as a *prototype shortcut
on the dev/root box*, seed `store.json` with `{remoteId, secret}` and HMAC-sign a
`/:remote_id/gui_xml` request to prove the render loop end-to-end (decoupled from the
app). **Bonus artifact:** the decode also yields the full tr2 icon glyph map
(`button.*`/`control.*`/`navigation.*` → PUA codepoints) — useful for the §4b/POC
renderer.

### TWO client types + auth (settled 2026-07-21, this project has a Halo + 2 Core-1s)
| | **Remote** (Halo/Neeo/our keypad) | **On-screen nav** `CONTROLLER_GUI` (Core-1/EA/panel) |
|---|---|---|
| Channel | HTTP `:3001` (nginx→cerebellum) | local **socket** `/var/run/director.socket` (`buildSocketConnection`, `BOOTSTRAP_DRIVER_SOCKET`) |
| Identity | per-remote key from QR pairing | **controller common name** (`getControllerCommonName` ← `/etc/commonname`, set by `openssl_commonname` at boot) |
| Auth | **HMAC-signed** HTTP requests | trusted **local system peer** — no HMAC, no pairing |

⇒ The controller path is easier but **closed to a non-controller device** (needs a
provisioned Control4 controller identity running locally). **Our keypad is a
"remote" → QR + HMAC pairing is the route.** The `/gui/` "Test UI" is NOT open
(401 even locally; likely needs a `TEST_UI_ENABLED` dev flag — every probe logs
`INVALID_ROUTE … Unauthorized` in `cerebellum.log`).

### The Remote Manager is an AGENT with debug Actions (Composer-observed)
- **`Halo Remote Hub`** agent (= `control4-halo-remote-hub.c4z`, the
  `C4_MANAGING_DRIVER`) sits in the Agents list next to our **NuVoxel Agent**. Its
  **Actions** tab: **"Set JWT Token (debug)"** (single `JWT Token` string param →
  the hub authenticates with a **JWT**, almost certainly a Control4 **account/cloud**
  token), **"Force remote updates"**, **"Print all remote ID (debug)"** →
  `{"ST<remote-serial>":{"handle":257141588,"id":2697,"name":"Living Room Halo
  Remote"}}`. Neeo remote_id was `<remote-id>` (MAC <mac>, IP
  .94.155, fw 1.80.2-332a27ba; now deleted).
- So the auth stack is **hub JWT (account/cloud) → per-remote HMAC**. The hub's live
  JWT isn't in the project export / cerebellum store / mm.db (runtime/in-memory or in
  encrypted agent state) — decoding it (issuer/aud/expiry) is the way to learn whether
  the account layer is cloud-bound or director-mintable. **"Replace remote" = editing
  the driver's `Remote ID`** (the binding key) — re-points a driver instance at a
  different physical remote.

### Net dealer-less assessment
Dealer-less (homeowner) ✅ via the QR+app flow; but there's a **Control4
account/cloud dependency** (the hub JWT + app auth), so NOT cloud-independent. The
director-side add is dealer-less (cerebellum self-runs `AddDevice`); the open gates
remain **(1)** does the app/cloud accept a non-Control4 device, and **(2)** does
pairing install OUR driver. Both need a real-pairing capture (QR payload + app→cloud
calls) to answer.

## 5. First-party driver internals — `.c4l` are ELF, not encrypted Lua

- **`/control4/drivers/*.c4l` are compiled ELF 32-bit i386 shared objects**
  (`7f 45 4c 46`), NOT encrypted Lua. First-party drivers (`uidevice`,
  `UIDeviceGlassedge`, keypad_proxy, …) are native C++ linking
  `libdirector_driver.so`, running **director-side**. So there is **nothing to
  decrypt** — read them with `strings`/`objdump`. The AES in the director
  (`c4::crypto::decryptAES`, "Failed to decrypt driver state data") is only for
  encrypted driver **state/persist** data, not driver code. (Third-party `.c4z`
  can still ship squished/encrypted Lua — a different scheme — but the glass driver
  is native.) Copies saved to scratchpad; symbols/RTTI are intact (not stripped of
  demangled names). Local `objdump` (i386) can disassemble handlers if needed.

- **Complete native UI-device command vocabulary** (base `uidevice.c4l`):
  `SET_BRIGHTNESS`, `SET_ADAPTIVE_BRIGHTNESS`, `SET_SCREENSAVER_MODE`,
  `SET_SCREENSAVER_START_TIME`, `SET_SCREENSAVER_TIMEOUT`, `SET_SCREENSAVER_ENABLE`,
  `SET_SCREENSAVER_SETTINGS`, `SET_SCREENSAVER_CONFIG_PERSIST`,
  `SET_SCREENSAVER_GLOBAL_CONFIG_PERSIST`, `SET_WALLPAPER`, `SET_DARK_MODE`,
  `SET_DO_NOT_DISTURB`, `SET_NIGHT_SCHEDULE`, `SET_PROXIMITY_SENSOR`,
  `SET_MOTION_NOT_SENSED_INTERVAL`, `SET_RETURN_HOME_TIMEOUT`, `SET_FIT_TO_SCREEN`,
  `SET_TEMPERATURE_SOURCE`, `SET_NAVIGATOR_ROOM_FAVORITES`, `SET_DEVICE_SETTINGS`,
  `SET_TOUCHSCREEN_SETTINGS`, `SEND_URI`, `GET_SETUP`, `GET_STATE`. Events:
  `NOTIFY_SCREENSAVER_STARTED/STOPPED`, `NOTIFY_DARK_MODE_*`,
  `NOTIFY_PROXIMITY_SENSOR_*`, `NOTIFY_NIGHT_SCHEDULE_*`.
- **glassedge extends** the base with: `OnCmdSetBrightness`,
  `OnCmdSetAdaptiveBrightness`, `OnCmdMotionDetected`/`OnCmdMotionNotSensedInterval
  Changed` (proximity), `OnCmdSetCurrentTarget`/`SetMicEnabled`/`SetRegistered`/
  `SetRegistrationData`/`SetResponseData` (SIP intercom),
  `OnCmdSetVoiceInputConfig/Mode/TargetEnabled` (voice/Alexa), `OnCmdSetUiConfig`/
  `ClearUiConfig`, `OnCmdUpdateFirmware` (→ `SendCmdToDeviceAdministrator("otaupdate")`).
  A `proxy_configures_screensaver` capability flag gates whether screensaver config
  flows via the proxy.
- **`SEND_URI`/`OnCmdSendURI` rejects anything but `c4:navigator`/`navigator`**
  scheme (validated via `libboost_url`) → it's UI navigation, NOT an http/photo
  injection vector. `SET_WALLPAPER` exists (`current_wallpaper`/`WALLPAPER_CHANGED`)
  but its param is a wallpaper reference, not confirmed to accept an arbitrary URL.

### Net effect on the two questions
- **Cloud photos:** unchanged — no arbitrary-URL command; screensaver photos still
  come from the privileged `SystemData` push sourced from the media library.
- **Keypad-as-uidevice:** *stronger than first thought* — the screensaver/wallpaper/
  brightness/DND vocabulary is base-`uidevice`, so IF Gate B dispatch works we can
  receive and handle it in Lua; the only thing that's genuinely first-party is the
  Composer settings/programming UI surface. Gate B (does Director drive a
  third-party `uidevice` proxy) remains the one runtime unknown — spike pending.

## 6. Does the tr2 XML approach help our LVGL relayout / multi-size work?

**Short answer: no — not for responsive layout.** tr2 is worth mining for its
*data model and event vocabulary* (§4c), but it contributes **nothing** to the
"lay out different sizes/orientations" problem, and on that axis our firmware is
already ahead of it.

### Why tr2's XML doesn't help sizing/orientation
- **tr2 is not responsive.** Every `<screen>` in the 54 extracted templates is a
  hardcoded **480-px-wide portrait** canvas with absolute x/y/w/h coordinates
  (`<icon x="375" y="22" width="105" height="76">`, headers `height="30"`, etc.).
  There is **no** portrait/landscape/orientation/resolution/formFactor/media-query
  concept anywhere in the templates *or* the docs (grepped explicitly — zero hits).
- **The "adaptivity" is server-side theming, not layout.** The parameterized
  dimensions (`fastListScreen.default.height`, `header.button.width`, …) are
  Mustache vars filled from cerebellum's `guidata`/`startup.tpl.xml` theme dict —
  cerebellum injects numbers, but the captured values are all tuned to Control4's
  one 480-wide remote. That's theming a fixed layout, not reflowing it.
- **tr2 offloads all layout intelligence to a server.** The device is a *thin
  renderer* of pre-rendered screens. It "handles" one screen size the way a website
  ships a fixed 480-px design plus a server that only ever talks to one client.
- **Resolution mismatch anyway.** Our default S3 panel is 320×240; tr2's canvas is
  480 portrait. `nav_poc.c` already reflects the takeaway: it re-implements tr2's
  **`listContent`/`listItem` + event verbs** (`ChangeScreen`/`TriggerAction`/
  `RoomCommand`) as native LVGL widgets laid out for our screen, and deliberately
  does **not** consume tr2's pixel coordinates.

### Where our firmware already is (and it beats tr2 here)
`firmware-idf/main/ui.c` (LVGL 9.3, hand-written C) already does real
size/orientation adaptation that tr2 never had to:
- Reads `lv_display_get_horizontal/vertical_resolution()` at build time and picks a
  **flavor** — `x4L` (large landscape 2-col), `x4P` (narrow portrait), `x4Ls`/
  `smallP` (240-class) — across s3_lcdwiki 320×240, ws43 480×800, p4_nano 1280×800.
- Scales positions/fonts via `s_uiscale` + `ui_apply_font_scale` (`scale =
  shortSide/480`); runtime software rotation via `orient_to_rot()` /
  `bsp_apply_orientation`. Relayout is a teardown-and-rebuild (`ui_request_rebuild`
  → `doRebuild` → `lv_obj_clean` + `ui_begin` re-queries resolution).
- One `ui.c` shared across all boards (the Linux/T3 port symlinks it verbatim).

So the honest framing: **tr2 gives us structure + event semantics (already being
borrowed in `nav_poc.c`); the pixel layout is always re-derived natively per panel.
For responsive layout, keep improving `ui.c`'s flavor/`s_uiscale`/rebuild system —
there's nothing to inherit from Control4's XML.**

## 6b. LVGL's own XML engine — pros/cons if we ever want a declarative UI

Different beast from tr2 XML (different schema, not compatible). LVGL 9.3 ships a
runtime XML/declarative UI loader under
`firmware-idf/managed_components/lvgl__lvgl/src/others/xml/` — currently
**`LV_USE_XML 0` (disabled)** in our build. Assessment if we ever reconsider:

**What it can do**
- Declarative **component/widget tree** via Expat: `<component>` files with
  `<api>`/`<consts>`/`<styles>`/`<view>`; nest to any depth; `extends` base widgets.
- **Flex layout fully expressible** in XML (`flex_flow`, `flex_grow`, main/cross/
  track place), plus **`%` and `content` sizing** (`lv_pct`, `LV_SIZE_CONTENT`).
- **Rich styling** — ~100 props (sizing, pad, bg + gradients, border, shadow, text/
  font incl. `tiny_ttf`, transforms), named + inline styles with part/state selectors.
- **Subject/observer data-binding** (`bind_text`, `bind_checked`, `bind_flag_if_*`,
  `bind_state_if_*`; declared in a `<subjects>` section, `int`/`color`/`string`).
- **Event wiring** via `<lv_event-call_function trigger= callback=>` → a C function
  registered with `lv_xml_register_event_cb` (handlers stay in C).
- Load from **flash-embedded C strings** (`register_from_data`) *or* from files over
  an LVGL VFS (`register_from_file`).

**Cons / gaps that bite us**
- **Explicitly "open beta / experimental, not production ready"** (LVGL's own docs);
  off by default; docs lag the code (the "not supported yet" list is already stale).
- **No responsive/media-query layer** — same limitation as everything: `%`/flex-grow/
  `content`/align only; multi-resolution still means C logic on
  `LV_EVENT_RESOLUTION_CHANGED`. **It does not solve our multi-panel problem** any
  more than tr2 does.
- **No runtime-loadable `<screen>`** in the vendored source (only `<component>`/
  `<view>`); screens become top-level components in practice.
- **Grid only half-expressible** — you can place items in grid cells but there's no
  XML for the grid *track template* (`col_dsc`/`row_dsc` FR arrays).
- **Event handlers + any custom widget still require C.** XML only wires
  trigger→named-callback.
- **On-device cost:** enabling runtime XML pulls in full **Expat** (flash + heap;
  each instance re-parses its `<view>` string, string subjects hard-allocate 256 B).
  File loading also needs an `LV_USE_FS_*` VFS we don't currently configure.
- **Vendor-intended production flow is offline codegen** (LVGL UI Editor → export
  C/H), so the shipped binary is plain LVGL C and no parser ships — **but that editor
  is a separate desktop tool, not vendored in this repo.**

**Verdict:** not worth adopting now. It buys authoring ergonomics (declarative
layout/styles/binding) but **not** the thing we actually want (responsive multi-size),
it's beta, and either it bloats the ESP image with Expat or it forces an external
codegen tool into the build. If we ever want to move off ~1000 lines of manual
`s_uiscale` positioning, the realistic path is the **offline UI-Editor → export-C**
model (design-time XML, no runtime parser), not on-device XML — and even then the
per-panel flavor/scale logic stays in C. Revisit only if per-flavor hand-tuning in
`ui.c` becomes the maintenance bottleneck.
