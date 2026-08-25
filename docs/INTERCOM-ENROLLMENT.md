# Intercom enrollment — SOLVED (2026-07-21)

The SIP intercom now enrolls end-to-end: a rebooted keypad provisions via the
companion intercom driver and REGISTERs under its **agent-enrolled** identity
(`MMKeypad_<intercomId>`), verified live on two panels (3360, 3366)
panels. Tooling + Director access: our internal Director-access notes (not published).

## The two bugs that blocked enrollment (both fixed)

1. **Wrong proxy shape** (fixed by the split): a third-party `driver_arch_type=5`
   intercom is enrolled by Communication agent V2 **only when `intercomproxy` is the
   device's PRIMARY proxy**. The merged keypad made `keypad_proxy` primary → never
   enrolled. Fix: separate `NuVoxelKeypadIntercom.c4z` with intercomproxy primary
   (5001), agent-installed + relay-bound (binding 700). See below.

2. **Notifications sent to a dead binding** (the real final blocker):
   `intercom_proxy/intercom_notify.lua` still had `DEFAULT_PROXY_BINDINGID = 5003`
   — a leftover from the merged/combo driver, where intercomproxy was the 5003
   **sub**-proxy. In the split driver intercomproxy is PRIMARY at **5001**, so every
   `NOTIFY.*` (including `Sip_Username_Changed`, which is what tells the agent to
   create the FreeSWITCH directory entry) went to binding 5003 — **which doesn't
   exist here** — and the agent never heard it. Symptom: device REGISTERs but
   freeswitch.log loops `Can't find user [MMKeypad_<id>@…]`. Fix: set
   `DEFAULT_PROXY_BINDINGID = 5001` (matches `INTERCOM_PROXY` / driver.xml).

## Completing enrollment on the live Director (the operational steps)

After deploying the fixed driver, the agent must **re-discover** the endpoints
(it evaluated them while their notifys were misrouted):

1. Reboot the panel so it re-provisions cleanly (see our internal Director-access notes → "Rebooting
   a keypad device"). The device comes up blank and waits for the intercom to push
   SIP via the relay; it then REGISTERs (and loops `Can't find user` until enrolled).
2. `director.sh reload control4_communication_agent_v2.c4z` — the agent re-scans and
   enrolls every intercomproxy-primary endpoint that has fired `Sip_Username_Changed`
   to 5001. The device's next REGISTER then succeeds. Re-run this after each batch of
   panels provisions (it only enrolls endpoints already reporting a username).

**Self-heal after reload:** the intercom's `OnBindingChanged` does NOT re-fire for an
already-bound relay on driver reload / Director restart, which would leave `gKeypad`
nil ("no keypad link yet"). `OnDriverLateInit` now rediscovers the keypad via
`GetBoundProviderDevice(RELAY_BINDING)`, sends `MMK_WHOIS`, and calls `PushSipConfig`,
so the endpoint re-provisions without needing a device reboot.

---

## Appendix — original investigation (2026-07-20, merged driver)

Status of the SIP intercom after merging the companion intercom driver into the
keypad driver (`intercomproxy` was then sub-proxy 5003).

## What works

- **SIP registration.** Keypads register with the Director's FreeSWITCH (internal
  profile, `MMKeypad_<c4DeviceId>`). See [FREESWITCH-INTERCOM.md](../reference/control4/freeswitch-intercom.md).
- **Entitlement / license.** The driver fetches `/api/v1/fw/entitlement`, pushes a
  `tier=pro` token; the device applies it and `device_has_feature("intercom")` is
  true. (All 5 dev keypads resolve to pro via paid or active trial.)
- **UI visibility (fixed).** The intercom card is now gated on **local entitlement**
  (`MMK_HAS_SIP && device_has_feature("intercom")`), NOT on `s_nEps`, so it shows
  offline and the screen renders "No intercom targets" while the roster is empty
  (`ui.c` `ic_available()`).
- **Endpoint push (fixed).** `RefreshDoorStations` now re-runs after the device
  registers and no longer bails on a failed roster query, so at least the
  "Everyone" broadcast always reaches the panel (`intercom.lua`).

Net: the card appears and can announce to "Everyone".

## The open gap: the endpoint is not ENROLLED in the Communication agent (V2)

The panel shows **only "Everyone"** — it can't call door stations, groups, or
other keypads. Root cause: the keypad **self-provisions** its SIP account
(`[self-provisioned (no agent assignment)]` in director.log) and its roster query
comes back empty (`GET_DEVICE_LIST failed`). The Communication agent V2 never
enrolls/assigns our endpoint.

### ROOT CAUSE (found by the overnight spike — see INTERCOM-SPIKE-FINDINGS.md)

**A third-party (`driver_arch_type=5`) endpoint is enrolled by Communication agent
V2 only when `intercomproxy` is the device's PRIMARY proxy.** The merged
`NuVoxelKeypad.c4z` makes `keypad_proxy` primary and `intercomproxy` a *sub*-proxy,
so the agent classifies the device as a keypad and never assigns it a SIP identity
→ self-provisions → roster empty → "Everyone" only. Proof: both enrolled
third-party intercoms on this Director declare `intercomproxy primary="True"` —
Chowmain UniFi 3050 (intercom primary, doorstation sub) and the defunct old-split
3221 — while ours does not. (Note: the sub-proxy IS instantiated as a child item,
so "sub-proxy" alone isn't the blocker — Chowmain runs its intercom primary with a
working sub; it's specifically the KEYPAD being primary that stops enrollment.)
Native touch screens/DS2 enroll via a different NATIVE path, which is why the
earlier "proxy structure is fine" reasoning (generalizing from them) was wrong.

### Ruled out (with evidence)

- **`driver_arch_type`**: 5 is correct. Our own OLD SPLIT drivers
  (`MMKeypadIntercom`/`MediaKeypadIntercom`, which worked) use 5; only the native
  `control4_doorstation_ds2` uses 4. Not the regression.
- **INTERCOM connection**: present (id 5003, `<classname>INTERCOM</classname>`,
  facing 6, type 2) — identical to Control4's `intercom_universal` reference.
- **`Sip_Username_Changed` binding**: the vendored `send_notify` defaults to
  `DEFAULT_PROXY_BINDINGID = 5003`, so our SIP username IS reported to the intercom
  proxy, not the keypad proxy.
- **`GET_DEVICE_LIST`/`GET_GROUP_LIST` invented?** No — confirmed **real** V2
  commands (found in the agent's memory next to `GET_ENDPOINT_LIST`,
  `GET_SESSION_LIST`, `STATE_LIST`). They're synchronous `SendUIRequest`s; they
  return empty only because we're not enrolled.

### The key positive signal

The Communication agent's live roster (extracted from director memory,
`/proc/<director>/mem`) **contains a keypad**: `MMKeypad_3221@192.168.1.220`
tagged `Office · Media Keypad Intercom`, alongside `UniFi_3050_cpvu` and the gate
station. `3221` is a **defunct old instance** (the current Office keypad is a
different id). So **a keypad of exactly our type WAS enrolled** — enrollment is
achievable; the *current* instances (churned through many delete/re-adds during
the dedup work) are not enrolled.

### Where the answer lives

The enrollment gate is inside `control4_communication_agent_v2` — **encrypted**
(`driver.lua.encrypted`). Its decrypted Lua is in the Director's memory but
compiled/minified; the capability vocabulary is extractable
(`is_doorstation, has_dial_pad_ui, driver_arch_type, sipUserName, isMobileUser,
currentState, …`) but not clean decision logic.

## Leading hypotheses for the overnight spike

1. **Enrollment is stateful and the merge/churn broke it.** `3221` enrolled; the
   merged sub-proxy instances don't. Two shapes to try:
   - Rearrange the proxy (e.g. intercom as its own child driver again, or adjust
     the sub-proxy declaration), or
   - Re-instate the **old split driver** (separate intercom `.c4z`, intercomproxy
     PRIMARY) which is known to have enrolled, then diff its *runtime* behaviour
     (what it reports to the agent, in what order) against the merged driver.
2. **A missing init-time report.** The native/enrolled path may send the agent a
   specific notification (state/capability) on connect that the merged driver
   doesn't, or sends in the wrong order relative to SIP registration.

Determine the difference by capturing the exact proxy traffic (force-log
`ReceivedFromProxy` + every `NOTIFY.*`/`SendToProxy`) for a driver that DOES
enroll vs the merged one.

## How to inspect enrollment state
```sh
# is our endpoint in the agent's roster? (decrypted agent state lives in memory)
apps/mmkeypad/tools/director.sh ssh 'node /tmp/memscan.js'   # see the memscan scripts
# self-provisioned vs agent-assigned:
apps/mmkeypad/tools/director.sh ssh 'grep -a "NVKP PushSipConfig" /var/log/debug/director.log | tail'
```

## Fix implemented (2026-07-21): split into keypad + companion intercom, agent-managed

Per the spike, the intercom is now a **separate `intercomproxy`-primary driver**
(the only shape the Communication agent V2 enrolls), installed and bound by the
agent, gated on license:

- **`driver-intercom/` → `NuVoxelKeypadIntercom.c4z`** — `intercomproxy primary="True"`
  (`arch_type=5`), reaches the device by relaying SIP/call JSON through the keypad
  over control binding `700` (`MMK_TX`/`MMK_RX`/`MMK_HELLO`/`MMK_WHOIS`). Built from
  the proven old-split base that enrolled as `3221`.
- **`driver-keypad/`** — intercomproxy sub-proxy + `intercom.lua`/`intercom_proxy/`
  removed; keypad_proxy stays primary; added the `700` relay **provider** and a
  `BindIntercom` command.
- **Agent** — `EnsureIntercom(dev, keypadId)`: installs `NuVoxelKeypadIntercom.c4z`
  in the keypad's room when the roster's `intercomC4z` is set, records
  `gDeviceMap[hwid].intercomId`, then sends the keypad `BindIntercom{deviceId}` so the
  keypad does the `C4:Bind` (C4:Bind must be called by a party to the binding).
- **Server** — `/api/v1/org/roster` returns `intercomC4z` **only** when
  `effectiveTier(...).features` includes `intercom` (paid Pro or active trial).

### Deploy / migration (ATTENDED — do NOT run unattended)

Removing `intercomproxy` from `NuVoxelKeypad.c4z` is a proxy change: it corrupts an
*installed* instance on update. So the existing 5 keypads must be **deleted and
re-added** (they were churned today anyway). Steps, watched:

1. Build the drivers in the open checkout (`~/git/mmkeypad-oss`) and deploy them
   to the Director (`director.sh deploy`), OR add via Composer. (Historical: this
   step used to publish to an OTA catalog; that platform has been removed.)
2. Deploy the web change (roster `intercomC4z`) so the agent sees it.
3. Delete the 5 keypads (`director.sh rm`), then agent **SyncDevices** — it re-adds
   each keypad, and for the pro/trial ones installs + binds the intercom.
4. Verify a keypad's intercom enrolls: `MMKeypad_<id>` appears in the agent roster in
   enrolled format (`roster_scan.js` on `/proc/11886/mem`) and the intercom driver's
   log flips from `self-provisioned` to an agent-assigned identity; the panel can now
   call door stations / groups / other keypads (not just "Everyone").
5. If the keypad half degrades with a bound relay, that's the fallback risk noted in
   INTERCOM-SPIKE-FINDINGS.md; capture and adjust.
