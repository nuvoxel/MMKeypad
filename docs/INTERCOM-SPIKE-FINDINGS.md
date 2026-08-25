# Intercom enrollment spike — findings (2026-07-21)

Autonomous overnight investigation into **why the merged `NuVoxelKeypad.c4z`
intercom endpoint is not enrolled** by the Control4 Communication agent V2 (so it
can only announce to "Everyone" and can't call door stations / groups / other
keypads), and **what the old split driver did differently**.

**Bottom line:** the enrolling factor is the **primary proxy class of the device**.
The Communication agent V2 enrolls a *third-party* (`driver_arch_type=5`) endpoint
only when **`intercomproxy` is the device's PRIMARY proxy**. The old split driver
(and the Chowmain UniFi) make `intercomproxy` primary, so they enroll. The merged
keypad makes **`keypad_proxy` primary** and `intercomproxy` a *sub*-proxy, so the
agent classifies the device as a keypad, never as an intercom endpoint, and never
assigns it a SIP identity. This is proven by live project structure + the agent's
in-memory roster + the driver's own log, with **no counterexample** on the Director.

This was reached entirely **read-only**. No spike driver was deployed and no
project item was added — see "Why the deployment spike was not run" below. The
production Director is exactly as found: 5 keypads (3330/3336/3339/3342/3345),
agent 3224, `NuVoxelKeypad.c4z` at `2026.07.20.011DRV`.

---

## The decisive evidence

### 1. The correct hypothesis, and the one that was wrongly ruled out

`INTERCOM-ENROLLMENT.md` recorded proxy structure as **ruled out**, reasoning
"device-primary + `intercomproxy` sub-proxy IS the correct V2 pattern (same as
native touch screens / DS2)." **That conclusion was wrong for third-party
drivers.** The touch screen and DS2 are *native* devices on a different enrollment
path (DS2 is `arch_type=4`, doorstation-primary; touch screens are Navigator
endpoints the agent knows intrinsically). Neither is a third-party
(`arch_type=5`) endpoint, so neither validates the keypad-primary + intercom-sub
shape.

### 2. Live project tree — ours vs. the two enrolled references

Pulled via `director.sh project` (SOAP `GetProjectItems`). Device = type 6, proxy
child = type 7.

| Device (type 6) | Primary proxy (type 7) | Other proxy child | arch | Enrolled? |
|---|---|---|---|---|
| **Ours** — Media Keypad `3330` (`NuVoxelKeypad.c4z`) | `3331` **keypad_proxy** | `3332` intercomproxy "Media Keypad Intercom" | 5 | **NO** |
| Chowmain UniFi `3050` | `3052` **intercomproxy** (`primary="True"`) | `3051` doorstation | 5 | **YES** |
| DS2 door station `2221`/`2231` | `2222`/`2232` **doorstation** | intercom + camera subs | 4 | YES (native) |
| Old split `MediaKeypadIntercom` (defunct `3221`) | **intercomproxy** (`primary="True"`, only proxy) | — | 5 | **WAS YES** |

The five current keypads all have the same shape as `3330`:
`3330→3331/3332`, `3336→3338`, `3339→3341`, `3342→3344`, `3345→3347`
(keypad_proxy primary, intercomproxy sub). So the `intercomproxy` sub-proxy **is**
instantiated as a real child project item — it is not missing. It is simply not
the primary, and the device therefore presents as a keypad.

The two `<proxies>` blocks that matter, read live off the Director:

```
Chowmain (ENROLLED):   <proxy proxybindingid="5003">doorstation</proxy>
                       <proxy proxybindingid="5002" primary="True">intercomproxy</proxy>

Old split (ENROLLED):  <proxy proxybindingid="5001" primary="True">intercomproxy</proxy>

Merged keypad (NOT):   <proxy proxybindingid="5002" name="Media Keypad" primary="True">keypad_proxy</proxy>
                       <proxy proxybindingid="5003" name="Media Keypad Intercom">intercomproxy</proxy>
```

**Key correction to prior notes:** `intercomproxy` being a *sub*-proxy is NOT by
itself the blocker — the Chowmain has `intercomproxy` as PRIMARY and its
`doorstation` as the sub, and it enrolls. The blocker is that on the merged keypad
the primary is `keypad_proxy`. Every enrolled third-party intercom on this
Director has `intercomproxy` primary; **there is no enrolled third-party endpoint
whose primary is anything other than `intercomproxy`.**

### 3. The agent's in-memory roster contains only the OLD split keypad

Scanned `/proc/11886/mem` (Director hosts all agents) for every SIP identity and
its surrounding context. Only entries in the **enrolled-roster format** (AOR +
`intercom.control4.com:5081` cloud relay + `Room`Device` display tag) count as
enrolled:

```
MMKeypad_3221@192.168.1.220 ....  Office`Media Keypad Intercom     ← old split, enrolled
UniFi_3050_cpvu@192.168.1.220 ..  Front Yard`Gate - Entry          ← Chowmain, enrolled
C4DS2-... (door stations)                                          ← native, enrolled
```

The current keypads (`MMKeypad_3342`, `_3345`, `_3276`, `_3279`, `_3303`, `_3333`,
…) appear **only** in (a) driver log strings and (b) FreeSWITCH presence/registration
event data — **never** in the enrolled-roster format. `MMKeypad_3221` is the
defunct old-split instance; it is the single keypad that ever enrolled, and it did
so under the `intercomproxy`-primary shape.

### 4. The driver's own log confirms the endpoint is never assigned

`/var/log/debug/director.log`, every current keypad, repeatedly:

```
[Bedroom-->Media Keypad(3342)] NVKP PushSipConfig
    [self-provisioned (no agent assignment)] user=MMKeypad_3342 pass=set
```

`PushSipConfig` prefers `readAssignedCreds()` (the identity the agent assigns to an
enrolled endpoint, read from the intercomproxy's `GET_DEVICE` props) and only falls
back to self-provisioning + `NOTIFY.Sip_Username_Changed` when the agent hasn't
assigned one. It **always** falls back — i.e. the agent never populates the
sub-proxy with an assigned identity, because it never enrolled the device. This is
the runtime symptom of the structural cause above.

### 5. Agent decision logic (partial extraction)

`control4_communication_agent_v2.c4z` is encrypted; its decrypted Lua in memory is
**string-table obfuscated** (`if X==tbl[5] or X==tbl[245] then …`), so clean
decision logic is not recoverable by memory scan (confirmed again this run — the
readable vocabulary is there: `has_intercom`, `has_video_intercom`,
`driver_arch_type`, `GetProxyDevices()`, `IS_HIDDEN`, but branch targets are
indirected through a scrambled string table). The structural + roster + log
evidence is what carries the conclusion; the obfuscated Lua is consistent with it
(it keys on proxy/arch capabilities) but does not add a readable rule.

---

## Split vs merged — the behavioral diff

The device-wire protocol (`sip`/`callcfg`/`call`/`endpoints`/`doorstations`/`chime`)
is byte-identical between the split `MediaKeypadIntercom` and the merged
`intercom.lua` — the merge preserved all of it (verified by reading both). The
handshake toward the *proxy* (`NOTIFY.*`, `SendDeviceProps`, `GET_STATE`/`GET_DEVICE`
replies) is also the same code. The only material difference is **which proxy
binding it hangs off and whether that proxy is primary**:

| | Old split (enrolled) | Merged (not enrolled) |
|---|---|---|
| intercomproxy binding | 5001, **`primary="True"`** | 5003, **sub-proxy** |
| device's primary proxy | intercomproxy (device *is* an intercom) | keypad_proxy (device *is* a keypad) |
| reaches device via | control relay from keypad driver | in-process `Send()` on the same `:6700` link |
| `Sip_Username_Changed` | sent unconditionally on link-up | sent only in the self-provision fallback (always taken) |
| agent classification | intercom endpoint → enrolled | keypad → ignored by comms agent |

The relay-vs-in-process change is irrelevant to enrollment (both deliver the same
NOTIFYs). The `primary` change is the whole story.

---

## Why the deployment spike was not run

The task authorized a spike driver under a new name (`NuVoxelIntercomSpike.c4z`),
added as a project item and checked for enrollment. I made the judgment call to
**not deploy it**, because:

1. **The question is already answered** with three independent, converging
   read-only lines of evidence (project structure, agent roster, driver log) and a
   clean natural experiment already present on the box: the old split
   (`intercomproxy` primary) enrolled as `MMKeypad_3221`; the merged keypad
   (`keypad_proxy` primary) does not. A spike reproducing the split shape would
   only re-confirm what `3221` already proves.
2. **A spike that enrolled would inject a live callable endpoint into the family's
   Communication agent roster** (visible on their touch screens / app) on an
   unattended overnight run, and `AddDevice` over REST is an *unexercised* handler
   on this OS 4.2 box (per `our internal Director-access notes` it is "seen in the binary, not yet
   exercised") — a partial add could leave a ghost item. That is disruption risk
   to a live home for no new information, which the guardrails say to avoid.

Force-logging the merged driver on one keypad was likewise unnecessary: the log
already shows the endpoint is perpetually `self-provisioned (no agent assignment)`,
and the proxy handshake code is identical to the enrolled split driver's — the
delta is structural (primary proxy), not in the message exchange.

The spike **worth running (attended)** is not the split re-confirmation but the
*fix candidate* — see Option B below.

---

## Recommended fix

The merged driver deliberately made `keypad_proxy` primary so the device presents
as a keypad (native Composer button editor) — but that is exactly what disqualifies
it from intercom enrollment. Two viable paths, both requiring a **new driver
filename** (reordering/retyping proxies on an installed driver corrupts the
project — the standing rule in `driver.xml`):

### Option A — proven: revert the intercom to a companion driver (split shape)
Ship the intercom as a separate `.c4z` with `intercomproxy` as its **primary
(only) proxy**, reaching the device through a control relay from the keypad driver,
exactly like the retired `MediaKeypadIntercom` that enrolled as `3221`. Known to
work. Cost: two drivers per keypad and the relay plumbing the merge removed.

### Option B — preferred if validated: make `intercomproxy` primary in ONE driver
Ship a new combined driver (e.g. `NuVoxelKeypad2.c4z`) that keeps both proxies but
**flips primacy**: `intercomproxy` `primary="True"`, `keypad_proxy` demoted to a
sub-proxy. This is precisely the Chowmain shape (intercomproxy primary +
doorstation sub, both fully functional and enrolled), so there is strong precedent
that a multi-proxy driver with `intercomproxy` primary enrolls *and* keeps its
other proxy working. Keep the full `<capabilities>` block (the keypad caps were
added to stop Composer greying out Button Settings — that concern is about
capabilities, not primacy, so demoting keypad_proxy should not reintroduce it).

**Validation step (run attended, not overnight):** deploy `NuVoxelKeypad2.c4z`
with the flipped proxy order, add **one** instance in a spare room pointed at a
spare/self device, let it register SIP, and confirm its `MMKeypad_<id>` appears in
the agent roster in enrolled format (re-run `roster_scan.js` against
`/proc/11886/mem`) and that `readAssignedCreds()` starts returning an
agent-assigned identity (driver log flips from `self-provisioned` to
`agent-assigned`). Then confirm the keypad half (button editor, LEDs, now-playing)
still works with `keypad_proxy` as a sub. If the keypad half degrades as a
sub-proxy, fall back to Option A. Remove the test item with `director.sh rm` when
done.

---

## Remaining unknowns (top 2)

1. **Does `keypad_proxy` remain fully functional as a *sub*-proxy** (Option B)?
   Untested on this Director. Chowmain proves a non-intercom proxy (doorstation)
   works as a sub under an intercom primary, but `keypad_proxy` specifically has
   not been tried in the sub position, and the earlier greying-out of Button
   Settings was tied to this proxy. This is the one thing the attended Option-B
   spike must confirm.
2. **Exact agent rule** (whether it is literally "primary proxy class ∈
   {intercomproxy, doorstation}" vs. some equivalent arch/interface enumeration).
   The behavior is fully consistent with the primary-proxy rule and has no
   counterexample, but the agent's obfuscated Lua could not be decoded to quote the
   literal predicate. Not needed to act on the fix; noted for completeness.

---

## What was touched on the Director (all read-only)

- Reads: `director.sh project` (SOAP GetProjectItems), `fs_cli show registrations`,
  `/var/log/debug/director.log` (grep), `/proc/11886/mem` (node mem scans),
  `ls`/`grep` of `/mnt/internal/c4z/*` driver sources.
- Wrote two throwaway scanner scripts to `/tmp` on the Director
  (`roster_scan.js`, `agent_logic.js`) and **removed both** at the end. The
  pre-existing `memscan*.js`/`cmd.js`/`soap.js` were left as found.
- **No** driver deploy, **no** project item add/remove, **no** Director/FreeSWITCH
  restart, **no** commits. Production driver remains `2026.07.20.011DRV`; 5 keypads
  intact.
