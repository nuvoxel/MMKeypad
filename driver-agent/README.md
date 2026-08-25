# NuVoxel Agent (`NuVoxelAgent.c4z`)

The account-bound **management agent**. One instance per home. It holds the
customer's NuVoxel organization API key, reads the account's device roster from
the NuVoxel portal, and installs the correct per-device Control4 driver for each
device that isn't in the project yet.

This replaces per-keypad dealer installs: claim a device in the NuVoxel app,
press one Action in Composer, and the device driver appears in the project.

## Build

```sh
cd driver-agent && ./build.sh
```

Produces `NuVoxelAgent.c4z` and copies it to `~/Documents/Control4/Drivers`.
Version scheme is shared with the keypad driver (`tools/nvversion.sh`):
`YYYY.MM.DD.NNN` + `DRV`, with the integer form `YYMMDDNNN` written into
Composer's `<version>` so **Sync Local** sees each build as newer.

Requires Control4 OS **3.2.0 or newer** (`C4:AddDevice`).

## Install

1. In Composer Pro: **Agents → Add**, pick **NuVoxel Agent**. It is an agent,
   so it attaches to the project root, not to a room — there is nowhere to
   "place" it. Add it **once** per project; a second instance would race the
   first on the same roster.

   > **Add fresh — never update in place.** Before it was an agent this was a
   > room-placed device driver. The project item's type and parent both change,
   > which Composer's Update / Sync Local path cannot do. Delete any existing
   > instance and add this one from the Agents list.
2. Make sure the per-device drivers (e.g. `NuVoxelKeypad.c4z`) are in the
   Director's driver database — normally by dropping them into
   `~/Documents/Control4/Drivers`. This driver installs drivers *by filename*;
   it cannot install a `.c4z` the Director has never seen.

## The organization-key flow

1. In the NuVoxel portal, go to **Settings → API Keys** and create an
   **organization** key. It looks like `nv_org_…`.
2. Paste it into the driver's **Organization Key** property in Composer.
   The driver immediately fetches the roster to validate the key and reports the
   outcome in **Status**.
3. In the portal, open each keypad and choose its **Control4 room** (the driver
   has already sent the room list up — see [Rooms](#rooms)).
4. Press the **Sync Devices From Account** action.

The key authenticates as the whole organization (`x-api-key` header on
`GET {Cloud URL}/api/v1/org/roster`), which is what lets a Director that knows
nothing about the customer's account learn the full device list. Treat it as a
secret: anyone with it can read the account's roster.

## Properties

| Property | Meaning |
| --- | --- |
| Driver Version | Read-only build stamp. |
| Organization Key | The `nv_org_` API key. |
| Cloud URL | Portal base URL. Default `https://nuvoxel.com`. |
| Status | Result + time of the last roster fetch or sync. |
| Installed Devices | How many account devices this driver has installed. |
| Refresh Interval | How often the roster is re-read. **Refresh never installs.** |
| Debug Logging | Off / Print / Log / Print and Log. |

There is no "install into room" property — see [Rooms](#rooms).

## Rooms

Only the customer knows which room a keypad is physically in, so the room is
chosen **per device in the NuVoxel portal**, not here.

1. On every roster poll this driver enumerates the project's rooms
   (`C4:GetDevicesByC4iName("roomdevice.c4i")`) and POSTs them to
   `{Cloud URL}/api/v1/org/c4/rooms`.
2. On the keypad's page in the portal, **Add to Control4** offers that list.
3. The choice comes back on the roster as `installState` + `c4RoomId`, and
   **Sync Devices From Account** creates each device in its own room.

`installState` values, and what this driver does with each:

| Value | Behaviour |
| --- | --- |
| `assigned` | Create the device in `c4RoomId` (which must be > 0). |
| `skip` | Never install; counted as "not installable". |
| `unassigned` (or absent) | **Not installed.** Counted in Status as awaiting a room. |

A device with no room chosen is deliberately left alone. The roster's `c4RoomId`
is the only source of a room — there is no fallback, and as an agent this driver
has no room of its own to fall back to. The previous behaviour (defaulting to
whichever room the driver had been placed in) put every keypad in a house into
one room. A device that somehow reaches the install step without a room counts
as **failed** in Status rather than being placed by guesswork.

A retired fourth value, `driver-room`, meant "install into the manager driver's
own room". It is impossible for an agent; the portal no longer offers it and any
device still stored that way reads back as `unassigned`.

## Actions

- **Sync Devices From Account** — fetch the roster and install every missing
  installable device **that has a room assigned**. The only path that creates
  devices.
- **Refresh Roster (no install)** — fetch and report, e.g.
  `3 new device(s) available`.
- **Send Room List To Portal** — push the project's rooms immediately, instead
  of waiting for the next poll. Useful right after adding rooms in Composer.
- **Forget Installed Device Map** — clears the driver's record of what it
  installed. Only use after deleting those devices in Composer; otherwise the
  next Sync has to fall back on name matching.

### Why installing is behind an Action

Control4 documents `C4:AddDevice` / `C4:AddLocation` as operations that "should
only be initiated through user interaction from the Dealer or end user" — a
driver that adds drivers unattended can recursively add drivers. So the periodic
refresh timer is allowed to *look* and report a count in Status, but only the
Action installs.

## Idempotency

Sync is safe to press repeatedly. Two checks stop duplicates:

1. **Persisted map** (`C4:PersistSetValue`) of `hardwareId → Control4 device
   id`. Exact, survives reloads and Director restarts. Entries whose device no
   longer resolves are dropped so a Composer deletion re-installs cleanly.
2. **Defensive project scan** for a device created from the same `.c4z` whose
   display name matches the roster name. Covers a wiped persist store (driver
   deleted and re-added, project restored). It's a heuristic — a rename in
   Composer defeats it — but any match it finds is written back into the map, so
   it's needed at most once per device.

## Forward compatibility

The server decides which `.c4z` each device needs (`driverC4z` in the roster).
There is deliberately **no sku → driver table in this driver**, so a brand-new
NuVoxel product can ship and install without a driver update. Devices with an
empty `driverC4z` are skipped as "not installable" (not an error), and unknown
or missing roster fields are tolerated rather than rejected.

## Status messages

| Status | Meaning |
| --- | --- |
| `No Organization Key set…` | Paste the key. |
| `Organization Key rejected (401)…` | Wrong/revoked key. |
| `Cannot reach https://nuvoxel.com (…)` | DNS/route/TLS/timeout from the Director. |
| `Server error 5xx fetching roster` | Portal problem; retry. |
| `Account has no devices yet…` | Key is good, roster is empty — claim a device in the app. |
| `N new device(s) available…` | Run the Sync action. |
| `Synced N account device(s): …` | Per-run counts: installed / already present / not installable / failed. |
| `…; N device(s) awaiting room assignment in the portal` | Those devices have no room yet. Finish them in the portal, then Sync again. |
| `Sent N room(s) to the portal` | Result of the Send Room List action. |
| `No Control4 rooms found in this project` | Room enumeration returned nothing; the portal keeps whatever list it had. |

`failed` most often means the per-device `.c4z` isn't in the Director's driver
database — see step 2 of Install.
