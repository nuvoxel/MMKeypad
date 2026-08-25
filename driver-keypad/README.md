# NuVoxel Keypad — Control4 driver (per-device)

A DriverWorks **Lua** driver (`.c4z`) that runs on the Control4 Director. One
instance per keypad, placed in the keypad's room. The **device** is the TCP
server; this driver connects out to it over a network binding (6001 → :6700),
reads the room's now-playing state, relays transport/volume/source, and hosts the
keypad's SIP intercom endpoint. Wire format: [`../PROTOCOL.md`](../PROTOCOL.md).

## Why `NuVoxelKeypad.c4z` and not `MediaKeypad.c4z`

Control4 instantiates a driver's proxies **only when the driver is first added to
a project**. Changing the `<proxy>` set of an already-installed driver corrupts
the project — it has crashed a Director in this repo. This driver restructures
the proxy set (it absorbs the former companion intercom driver as a third proxy),
so it must ship under a filename that has never been installed anywhere. Do not
reuse the old name, and do not add/remove/retype a proxy in any future update to
*this* one either.

| Binding | Proxy | Role |
|---------|-------|------|
| 5002 | `keypad` (**primary**) | Programmable on-screen buttons + RGB LEDs |
| 5003 | `intercomproxy` | SIP intercom endpoint (`intercom.lua`) |
| 5001 | *(network binding only, no proxy)* | Placeable driver shell; owns the network binding |

`ReceivedFromProxy(idBinding, …)` dispatches on the binding id. Composer renders
this as a parent/child tree: the primary is the device, the rest are sub-proxies.

## Files

| File | Purpose |
|------|---------|
| `driver.xml` | Manifest: properties, proxies, connections, actions, events. |
| `driver.lua` | Lifecycle, room read/poll, command dispatch, protocol, portal settings mirror. |
| `intercom.lua` | The intercom endpoint (proxy 5003) — SIP provisioning, call state, levels. |
| `intercom_proxy/` | Vendored Control4 intercom proxy contract (constants/notify/command/protocol/debug). |
| `json.lua` | Bundled JSON encode/decode (avoids `C4:JsonEncode` quirks). |
| `www/documentation.html` | Required in-Composer help. |
| `icons/` | Device icons. |
| `build.sh` | Packages everything into `NuVoxelKeypad.c4z`. |

## Build & install

```bash
./build.sh                 # -> NuVoxelKeypad.c4z (also copied to ~/Documents/Control4/Drivers)
```

Then in **Composer Pro**: the keypad advertises over SDDP, so add it from
discovered devices — or *System Design → Search → Add Driver* and bind its
**Media Keypad Network** connection to the device.

For dev reloads use **Drivers → Manage Drivers → Sync Local** (`build.sh` bumps
`<version>` every build, so Sync Local sees it as newer). That is safe for code
and property changes only; anything touching the proxy set needs a new filename.

## Settings ownership

Settings are **portal-authoritative**. The nuvoxel portal owns the configuration
and the driver mirrors it both ways: it pulls on device connect and on the
existing 10-minute entitlement poll, and pushes back when a dealer edits a
property in Composer. On conflict the portal wins (a push carrying a stale
`settingsRev` returns 409 with the winning state, which the driver re-applies).

`null` in the portal means "no opinion", which the driver renders as the existing
`-1` sentinel the firmware already skips — the same thing "Auto (device setting)"
means in Composer. The device protocol is unchanged by any of this.

Because this is a *per-device* driver it holds only device credentials
(`X-Device-Id` / `X-Device-Secret` from the keypad's `hello`), never an org API
key, so settings ride the device-authed entitlement exchange rather than the
org-authed `/api/v1/org/device/<hwid>/settings` endpoints.

## Needs a live Director to confirm

- Multi-proxy `ReceivedFromProxy` dispatch (5002 / 5003 / 301–306).
- The intercom fold-in as a **non-primary** proxy: enrollment in the
  Communications agent, and `C4:GetProxyDevices()`'s return shape with three
  proxies (`ProxyDeviceId()` in `intercom.lua` handles both shapes defensively).
- The `<capabilities>` block applying to the intercom sub-proxy of a multi-proxy driver
  driver.
- The portal settings mirror end-to-end — the server must serve `settings` /
  `settingsRev` on the entitlement response first; it does not today.
