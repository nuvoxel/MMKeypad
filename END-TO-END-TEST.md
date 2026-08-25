# NuVoxel keypad — end-to-end test checklist

Everything below is built + pushed but UNVERIFIED against a live Director. This is
the list of things only a real Director + hardware can confirm. Ordered so each
step unblocks the next. Diagnostics sink (from earlier spikes) is the dev Mac at
`http://192.168.46.146:8899` — set a driver's `Diagnostics Sink` property to it to
capture a driver's own trace.

## A. Core onboarding path (the main flow)

1. **Install the agent.** Composer → Agents → add **NuVoxel Agent** (`driver-agent/`
   builds `NuVoxelAgent.c4z`, already in `~/Documents/Control4/Drivers`). It should
   land under Agents (system root), not in a room.
   - CONFIRM: it appears, no error, Settings/Properties readable.

2. **Link by OAuth device flow.** Agent should auto-start linking on load: its Status
   / `Link Code` property shows a code + `nuvoxel.com/link`. Open that URL (signed in),
   enter the code, pick the org, approve.
   - CONFIRM: agent Status flips to "Linked to <org>"; the org key is persisted (survives
     a Sync Local). Migration 0025 is applied (done), so the endpoints work.
   - FALLBACK if device flow misbehaves: paste an `nv_org_…` key into `Organization Key`.

3. **Claim a keypad (QR/pairing — the path that works today).** Power on an S3 keypad.
   Unregistered, it shows a pairing code / QR on screen. Claim it in the portal
   (scan QR, or enter the code).
   - CONFIRM: device appears in the portal roster.

4. **Assign a room + install.** In the portal device page → "Add to Control4" → pick a
   room. Then in Composer run the agent Action **Sync Devices From Account**.
   - CONFIRM (the two biggest agent unknowns): the agent CAN call `C4:AddDevice` at all,
     and the keypad lands in the CHOSEN room (not the agent's / not "no room"). Read the
     result: `<director-shell> '/control4/bin/dman project list -f NuVoxelKeypad.c4z'` then
     `dman project list -p <deviceId>` — expect a DEVICE parent with `keypad_proxy.c4i` +
     `intercomproxy.c4i` children.

5. **Licensing applies automatically.** Once the keypad driver connects, `License Status`
   should show the tier (e.g. "Pro trial · N days"). This chain was already seen working.

## B. Keypad driver live-verifies

6. **Buttons above 6 survive a Director reboot (#17 — the riskiest line).** On a board that
   reports >6 buttons (nano=12, ws43=8), wire a button link, then REBOOT the Director.
   - CONFIRM: the dynamic BUTTON_LINK binding survived (button still actuates its target).
     If it comes back UNCONNECTED, SDK issue #8 applies to restore and it's a blocker.

7. **Button config survives without the portal (#18).** Set the keypad's `Diagnostics Sink`,
   Sync Local, and watch: does `keypad_proxy` send `KEYPAD_BUTTON_INFO` / `BUTTON_LIST_INFO`
   UNPROMPTED at init? If yes, C4 is the store and button config is portal-independent. If
   no, we need a local cache. (This is why button config removal is currently portal-backed.)

8. **Native keypad editor round-trips.** Edit a button in Composer's native Button Settings
   panel; confirm it reaches the device AND shows in the portal (last-write-wins). Then edit
   in the portal; confirm it reaches the device AND the native panel. Watch the sink for the
   loop-breaker (no infinite revision trading).

9b. **MediaSession push (#20).** Set the keypad's `Diagnostics Sink`; change now-playing /
   volume from another source. If events 104-109 fire, the driver should PushState within
   ~150ms (not the 1s poll) and the poll should recede to a 10s heartbeat. If they never
   fire, polling stays at 1s — either way now-playing stays correct. (Confirm nothing
   REGRESSED vs the old poll-only behavior; the events are pure upside.)

9. **Intercom proxy delivers (already seen once).** Toggle the intercom child's settings; the
   `SET_*` commands should arrive (sink trace). Confirms multi-proxy dispatch on 5003.

## D. Parked (specced, NOT built — need the loop above first)

- #13 layout builder (slot heights/rockers) — 5-board firmware grid rewrite; build with
  hardware in the loop.
- #19 security partition UI (variable listeners) — needs a security panel in the project.
- #28 agent HTML settings tab — confirmed possible (unsigned agents can); React build.
- #15 dealer-free button binding — blocked on the #17 reboot result (same AddDynamicBinding
  mechanism + SDK #8 risk).

## E. Cleanup after testing

- Remove both spikes from Composer: **NuVoxel uidevice Spike** (if still present),
  **NuVoxel Agent SDDP Spike** (the server spike was retired — it crashed the Director on removal; see memory c4-agent-html-ui).
