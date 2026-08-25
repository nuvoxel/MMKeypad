# tr2 navigator research — artifacts + scripts

> ⚠️ **Superseded for the media-keypad data plane (2026-07-22).** For now-playing +
> source control the keypad does NOT need tr2/cerebellum — it uses native MediaSession
> events + room commands (`SendToDevice(roomId,…)`); only deep *catalog browse* would
> use the MSP navigator. See [`../../../docs/DATA-PLANE.md`](../../../docs/DATA-PLANE.md)
> and [`../media-commands.md`](../media-commands.md). This tr2 work
> remains the reference for the separate **dealer-less full-navigator** ambition only.

Everything from reverse-engineering Control4's **tr2 navigator** (the UI/protocol
that Neeo/Halo/on-screen navigators render) toward a **dealer-less NuVoxel navigator
keypad**. The narrative + findings live in
[`../screensaver-and-uidevice.md`](../screensaver-and-uidevice.md)
(§4b–§4d) and our internal Director-access notes (not published). This folder holds the
reproducible bits.

## TL;DR of what we found
- **TR2** = the navigator UI (declarative Mustache-XML templates, served over HTTP
  `:3001` / the `:3004` message channel). **TR3** = the node services that generate
  it (`cerebellum`, `imageservice`). cerebellum renders TR2.
- **Pairing = direct LAN self-register** (device discovers director → user picks
  director + room → `AddRemote`). No dealer, no Composer, no cloud, no QR (that's the
  Halo variant).
- **Auth = a director-MINTED JWT** (RS256, system-user, `/remote,/director,/concierge,
  /lightning` perms, ~30-min life) pushed to the device via the **`Token`** command.
  Minted locally by the director — cloud-independent.
- **Protocol = newline JSON**, `{command|reply, type, messageId, remoteId, status,
  ...}`. Browse = `GetItems{listId,firstItem,numItems}` → `NewList{...}`.

## Files here
- **`tr2-protocol-capture.txt`** — 124 unique live protocol messages captured from
  director memory (JWTs redacted). The message set our firmware implements.
- **`tr2-navigator/`** — Control4's tr2 UI template set (Mustache-XML: `gui.tpl.xml`,
  `list-dynamic.tpl.xml`, `listitem.tpl.xml`, `gui/keypad.tpl.xml`, `guidata/`, …).
  The render spec. Source on director: `/opt/control4/tr3/cerebellum/filerepo/tr2/`.
- **`scripts/memscan.js`** — scan a running process's `/proc/<pid>/mem` for string
  signatures (decrypts loaded driver Lua + captures live protocol). Runs on the
  director (has `node`). `node memscan.js <pid> '<sig1>' '<sig2>' ...`; get the pid
  from `/var/run/director.pid` (NOT `pgrep -f`, which matches itself).
- **`scripts/memscan2.js`** — same, but extracts complete JSON protocol messages +
  Lua chunks to `/tmp/proto_msgs.txt` / `/tmp/proto_lua.txt`.
- **`scripts/decode-c4-node.py`** — deobfuscate a tr3 node service (base64
  string-array) → cleartext identifiers/endpoints.

## Proprietary binaries — NOT committed (re-fetch as needed)
Control4-proprietary; kept out of git. Re-pull from the director (see our internal Director-access notes
for SSH):
```sh
# tr3 node services (base64-obfuscated) → decode with decode-c4-node.py
scp root@<dir>:/opt/control4/tr3/cerebellum/cerebellum.js .
# native first-party drivers (ELF, strings/objdump — not encrypted)
scp root@<dir>:/control4/drivers/uidevice.c4l root@<dir>:/control4/drivers/uidevice_glassedge.c4l .
# the LuaJIT engine (holds the obfuscated DriverWorks-Encryption key)
scp root@<dir>:/control4/drivers/lua_gen_jit.c4l .
# encrypted 3rd-party driver Lua (PKCS#7 → DriverWorks Encryption V2)
scp root@<dir>:/mnt/internal/c4z/control4_remote_hub/driver.lua.encrypted .
# tr2 templates
scp -r root@<dir>:/opt/control4/tr3/cerebellum/filerepo/tr2 .
```
Decrypting `.c4z` encrypted Lua: don't crack the key — **memory-scrape the running
director** (`memscan.js`); it keeps decrypted squished-Lua source resident.
