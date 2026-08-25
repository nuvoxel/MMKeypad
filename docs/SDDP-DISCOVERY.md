# Keypad discovery + OTA robustness — design note

> Written 2026-07-22 after a live diagnosis: 3 of 5 panels weren't bound to the
> driver, and the 2 T3s hadn't taken the `2026.07.22.001FW` OTA. Both trace to the
> same weakness — discovery/OTA that depends on mechanisms that are unreliable on a
> real home network. This note proposes the durable fixes.

## What we observed (dev fleet, 2026-07-22)

- Director is on a flat **/16** (`192.168.1.220/16`); the 5 panels are DHCP-assigned
  across it (`.70`, `.119`, `.152`, `.174`, `.223`) — i.e. on several WiFi APs.
- **All 5 panels were up and listening on `:6700`** and present in the director's ARP
  table. The devices were never the problem.
- **SDDP multicast discovery is unreliable here:** 3 panels (Office, Kitchen-T3,
  Playroom-T3) were never discovered/bound; one panel held a **stale** binding to an IP
  its device had moved off of (DHCP). `nc :6700` to the panels *flapped* open/closed
  between probes — the WiFi path is intermittent, and multicast (SDDP relies on it)
  forwards poorly across APs.
- Manually pointing each driver at its device's real IP (`SetBindingAddress` +
  `NetConnect`) connected them instantly — proving the connect path is fine and the
  fault is purely **discovery**.
- **OTA gap:** ESP panels self-updated (autonomous cloud OTA client); the 2 T3s did
  not — they depend on the driver's manual `{t="ota"}` trigger, which they never got
  while unbound. Manual trigger → they updated.

## Root causes

1. **Discovery leans on multicast SDDP**, which is flaky across WiFi/APs and silently
   fails — no fallback, no self-heal when DHCP moves a device.
2. **No DHCP stability** — device IPs move, so even a once-good binding goes stale.
3. **T3 OTA is trigger-dependent** — no autonomous cloud check like the ESP client, so
   a disconnected/never-triggered T3 stays behind indefinitely.
4. **Driver OTA is manual-only** — fires on the "Update Firmware" command; no auto
   check on connect and no timer.

## Fix 1 (immediate, network-side): DHCP reservations

Reserve the 5 keypad MACs on the router. Pins IPs so bindings never go stale and the
whole "device moved" failure mode disappears. Cheapest, no code. Recommended regardless
of the driver work below.

## Fix 2 (durable): agent MAC→IP handoff — cloud-mediated, bypasses multicast — IMPLEMENTED 2026-07-22

Control4 Lua can't read `/proc/net/arp`, so the agent resolves IP via the **cloud**
instead — which is *better*, because the pieces already exist:

- **The device already reports its own LAN IP to the cloud on check-in** (`report.status.ip`,
  stored on `platform.stations.ip_address` — commit 7a143a4). E.g. Playroom → `192.168.152.79`.
- **The agent already fetches a cloud roster** (`GET /api/v1/org/roster`) keyed by hardwareId.

**Implemented flow (device → cloud → agent → keypad, all over reliable HTTPS, no multicast):**
1. Web roster route now includes each device's stored `lanIp` (`route.ts`).
2. Agent, on **every** sync, pushes `C4:SendToDevice(keypadId, "NV_SET_TARGET_IP", {ip})`
   for each installed keypad that has a `lanIp` (driver-agent/driver.lua, in the
   present-device branch of `SyncDevices`).
3. Keypad driver handles `NV_SET_TARGET_IP` → `SetTargetIp(ip)` = `SetBindingAddress(6001, ip)`
   + `NetConnect` (the exact pair that worked manually). Idempotent: no-op if already
   connected there (driver-keypad/driver.lua). SDDP + `hello` remain the fallback.

**Why robust:** both device and agent reach the cloud regardless of subnet/AP/multicast.
DHCP moves self-heal — the device re-checks-in with its new IP, the roster updates, the
next agent sync re-pushes. Freshness = device check-in cadence (≤6h, sooner on DHCP
change/retry) + agent roster poll. **Surface:** additive only (`NV_SET_TARGET_IP` mirrors
`NV_SET_TARGET_MAC`; roster gains a field) — FROZEN-safe.

**To go live:** deploy agent + keypad driver to the Director, and deploy the web change
to prod (roster must return `lanIp`).

## Fix 3 (T3 parity): autonomous OTA check on the T3

Port the ESP device client's autonomous cloud-OTA check to the T3 (`nv_ota_t3.c` /
`device_t3.c`) so a T3 self-updates on boot + a timer, independent of the driver
trigger. Today a disconnected/un-triggered T3 never updates (it sat on the prior build
while every ESP panel self-updated).

## Fix 4 (nice-to-have): driver auto-OTA on connect

Have the keypad driver fire an OTA check automatically when a device connects (or on a
slow timer), not only on the manual "Update Firmware" command — so panels converge to
the published firmware on their own once reachable.

## Priority

1. DHCP reservations (now, no code) — stops the churn.
2. Agent IP-handoff (Fix 2) — the real discovery fix; small, additive, FROZEN-safe.
3. T3 autonomous OTA (Fix 3) — closes the "T3 silently stays behind" gap.
4. Driver auto-OTA-on-connect (Fix 4) — convenience.
