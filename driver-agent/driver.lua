--[[ ===========================================================================
  NuVoxel Agent — Control4 DriverWorks AGENT
  ---------------------------------------------------------------------------
  ONE instance per home, living at the project root (see driver.xml for why an
  agent and not a room-placed device). The dealer (or customer) pastes their
  NuVoxel organization API key; this driver then knows the whole account and
  can install the right per-device driver for every device the customer owns —
  so adding a keypad becomes "claim it in the app, press Sync here" instead of a
  dealer hand-installing and hand-configuring a driver per keypad.

  Roster contract (server-supplied, deliberately flat):
    GET {Cloud URL}/api/v1/org/roster        header: x-api-key: nv_org_…
    -> { devices: [ { hardwareId, kind, deviceSku, name, model, online,
                      firmwareVersion, driverC4z, c4RoomId, installState } ] }

  The SERVER decides which .c4z belongs to each device (driverC4z). That is the
  whole point: a brand-new NuVoxel product can ship and be installed by this
  driver without this driver ever being updated. Never build a sku -> c4z table
  here, and never reject a device for having fields we don't recognise.

  Rooms — the roster is the ONLY source
  ---------------------------------------------------------------------------
  This driver can enumerate the project's rooms but has no idea which one a
  given keypad is physically in; the customer does. So on every roster poll we
  push the room list UP (POST /api/v1/org/c4/rooms), the customer picks a room
  per device in the portal, and the roster hands the choice back down as
  installState + c4RoomId. A device with no choice is NOT installed — it is
  counted in Status instead.

  There is deliberately no fallback room. As an agent this driver has no room of
  its own to fall back TO, and that is the point: the previous room-placed
  version defaulted to C4:RoomGetId(), so a house with keypads in eight rooms
  got eight devices in whichever room a dealer had dropped the driver. A device
  we cannot place is a loud Status line, never a guess.

  installState values (anything else, including absent, means "unassigned"):
    assigned    -> create in c4RoomId (which must be > 0)
    skip        -> deliberately not a Control4 device; never install, never
                   report as waiting
    unassigned  -> nobody has chosen yet; do not install

  Control4 API notes:
    * C4:AddDevice(strC4zName, nRoomId, strName, callback) -- OS 3.2.0+
        callback signature: function(deviceId, tDeviceInfo). Returns 0 on
        failure. For proxy devices tDeviceInfo carries every proxy device id
        plus the protocol driver id.
      UNVERIFIED: whether an AGENT may call AddDevice at all. The API is
      documented on C4 (the global proxy object every driver has) with no stated
      restriction to room-placed drivers, and nRoomId is an explicit argument
      rather than something derived from the caller — so an agent has no less
      information than a device driver does. But this has not been run on a live
      Director from an agent. If it turns out agents are barred, AddDevice
      returns 0 and the immediate-failure path below reports it in Status rather
      than failing silently — which is exactly how we would find out.
    * C4:AddDevice/C4:AddLocation are documented as "should only be initiated
      through user interaction from the Dealer or end user" — a driver that adds
      devices on its own can recursively add drivers that add drivers. Hence:
      installing is ONLY ever reachable from the Sync action. The refresh timer
      is allowed to LOOK (and report a count) but must never install.
    * C4:PersistSetValue / C4:PersistGetValue survive driver reloads + Director
      restarts, but NOT a driver delete/re-add or a project rebuild — see the
      reconcile notes on FindExistingDevice().
    * Don't call network/timer/JSON APIs in OnDriverInit.
=========================================================================== --]]

DRIVER_VERSION  = "dev"   -- placeholder; build.sh stamps the real version (version.txt) into the .c4z
PERSIST_MAP     = "nv_sys_device_map"    -- hardwareId -> installed Control4 device id
-- The org API key obtained by the device-flow link (RFC 8628). Persisted rather
-- than written into a Property so it survives reloads/restarts and is NEVER
-- shown in Composer — the pasted "Organization Key" property is the visible,
-- headless-fallback path; this is the primary, browser-approved one.
PERSIST_KEY      = "nv_sys_org_key"
-- A stable id for THIS install, generated once. Sent as instanceId on link/start
-- (support signal only, never trusted for auth), mirroring the activate ping.
PERSIST_INSTANCE = "nv_sys_instance_id"

print("NuVoxelAgent: driver.lua loading v" .. DRIVER_VERSION)

-- Robust JSON load (bundled lib, else C4 built-in fallback so the driver always loads).
local json
do
  for _, name in ipairs({ "json", "lib.json" }) do
    local ok, mod = pcall(require, name)
    if ok and type(mod) == "table" and mod.encode then json = mod; break end
  end
  if not json then
    json = { encode = function(v) return C4:JsonEncode(v, false, true) end,
             decode = function(s) return C4:JsonDecode(s) end }
  end
end

-- Runtime state -------------------------------------------------------------
local gRefreshTimer = nil
local gRoster       = nil   -- last successful roster (array of device tables)
local gDeviceMap    = {}    -- hardwareId -> { id = <c4 device id>, c4z = ..., name = ... }
local gSyncing      = false -- guards against a second Sync while AddDevice callbacks are in flight
local gDriverInstallTried = {} -- c4z -> true: installed a missing driver this load (bounds retries)

-- Device-flow linking state (RFC 8628). All in memory: a link in progress is
-- cheap to restart, so a reload mid-link just re-begins rather than resuming.
local gLinkTimer      = nil   -- repeating poll timer
local gLinkDeviceCode = nil   -- the secret we poll with; nil when not linking
local gLinkIntervalMs = 5000  -- server-advised poll cadence
local gLinkPolling    = false -- guards against stacking polls if one is slow

local REFRESH_MS = {
  ["Off"]          = 0,
  ["15 minutes"]   = 15 * 60 * 1000,
  ["1 hour"]       = 60 * 60 * 1000,
  ["6 hours"]      = 6 * 60 * 60 * 1000,
  ["24 hours"]     = 24 * 60 * 60 * 1000,
}

-- ============================================================================
-- Logging / small helpers
-- ============================================================================
local function dbg(...)
  local mode = Properties and Properties["Debug Logging"] or "Print"
  if mode == "Off" then return end
  local parts = {}
  for i = 1, select("#", ...) do parts[#parts + 1] = tostring((select(i, ...))) end
  local msg = "NuVoxelAgent: " .. table.concat(parts, " ")
  -- Cap the line: some of what passes through here is cloud- or device-supplied,
  -- and with Debug Logging = Log it all lands in the Director's error log.
  if #msg > 400 then msg = msg:sub(1, 400) .. "…(+" .. (#msg - 400) .. ")" end
  if mode == "Print" or mode == "Print and Log" then print(msg) end
  if (mode == "Log" or mode == "Print and Log") and C4.ErrorLog then C4:ErrorLog(msg) end
end

-- SetTimer returns a timer object (:Cancel) on current cores but a numeric handle on
-- others — cancel either form so nothing keeps firing (esp. across driver destroy).
local function cancelTimer(t)
  if not t then return end
  if type(t) == "table" and t.Cancel then pcall(function() t:Cancel() end)
  else pcall(function() C4:KillTimer(t) end) end
end

local function now()
  local ok, s = pcall(function() return os.date("%H:%M:%S") end)
  return (ok and s) or "?"
end

-- Every Status write goes through here so the dealer always sees WHEN, not just
-- what — a stale "Synced: 3 devices" is otherwise indistinguishable from a fresh one.
local function setStatus(s)
  pcall(function() C4:UpdateProperty("Status", tostring(s) .. "  (" .. now() .. ")") end)
  dbg("status:", s)
end

local function fireEvent(name)
  pcall(function() C4:FireEvent(name) end)
end

-- The ORGANIZATION API key is sent to this host, which makes it the most sensitive
-- destination either driver has. The property is read-only with a fixed default and
-- nothing writes it, so this is defence in depth rather than a live hole: validate
-- anyway, so the key can never be posted over http:// or to a host that is not ours
-- if that ever changes.
local CLOUD_DEFAULT     = "https://nuvoxel.com"
local CLOUD_HOST_SUFFIX = "nuvoxel.com"
function CloudUrl()
  local u    = Properties and tostring(Properties["Cloud URL"] or "") or ""
  local host = u:match("^https://([^/]+)")          -- https ONLY
  if host then
    host = host:gsub(":%d+$", ""):lower()
    if host == CLOUD_HOST_SUFFIX or
       host:sub(-(#CLOUD_HOST_SUFFIX + 1)) == "." .. CLOUD_HOST_SUFFIX then
      return (u:gsub("/+$", ""))
    end
    dbg("ignoring Cloud URL (not https under " .. CLOUD_HOST_SUFFIX .. "):", u)
  elseif u ~= "" then
    dbg("ignoring Cloud URL (not https):", u)
  end
  return CLOUD_DEFAULT
end

-- Persist helpers: values survive reloads + Director restarts (but not a driver
-- delete/re-add). Always strings — nested-table Persist round-trips aren't
-- guaranteed across cores (same reason the device map is JSON-encoded).
local function persistGet(k)
  local v
  pcall(function() v = C4:PersistGetValue(k) end)
  return (type(v) == "string") and v or ""
end
local function persistSet(k, v)
  pcall(function() C4:PersistSetValue(k, tostring(v)) end)
end

-- The effective org key. The pasted "Organization Key" property wins when set
-- (headless installs, debugging, or a dealer who prefers pasting); otherwise the
-- key obtained by the device-flow link. Device flow is the primary path, the
-- property is the deliberate fallback — so everything downstream (roster fetch,
-- room push) just calls this and never cares which path produced the key.
local function orgKey()
  local k = Properties and tostring(Properties["Organization Key"] or "") or ""
  k = k:gsub("^%s+", ""):gsub("%s+$", "")
  if k ~= "" then return k end
  return persistGet(PERSIST_KEY)   -- browser-approved key, or "" when never linked
end

-- Stable per-install id, generated once and persisted. Prefer the Director's own
-- device id (unique per install); fall back to a timestamp if it's unavailable.
local function instanceId()
  local id = persistGet(PERSIST_INSTANCE)
  if id ~= "" then return id end
  local okId, myId = pcall(function() return C4:GetDeviceID() end)
  id = "c4sys-" .. tostring((okId and myId and myId ~= 0) and myId or os.time())
  persistSet(PERSIST_INSTANCE, id)
  return id
end

-- ============================================================================
-- Persisted hardwareId -> Control4 device id map
-- ----------------------------------------------------------------------------
-- Stored JSON-encoded rather than as a raw table: Persist round-trips of nested
-- tables are not guaranteed across cores/OS versions, and a string always is.
-- ============================================================================
local function loadMap()
  local raw
  pcall(function() raw = C4:PersistGetValue(PERSIST_MAP) end)
  if type(raw) ~= "string" or raw == "" then return {} end
  local ok, t = pcall(json.decode, raw)
  return (ok and type(t) == "table") and t or {}
end

local function saveMap()
  local ok, raw = pcall(json.encode, gDeviceMap)
  if ok then pcall(function() C4:PersistSetValue(PERSIST_MAP, raw) end) end
end

local function mapCount()
  local n = 0
  for _ in pairs(gDeviceMap) do n = n + 1 end
  return n
end

local function updateCountProperty()
  pcall(function() C4:UpdateProperty("Installed Devices", tostring(mapCount())) end)
end

-- ============================================================================
-- Project inspection
-- ============================================================================
local function displayName(deviceId)
  local id = tonumber(deviceId)
  if not id or id == 0 then return "" end
  local ok, n = pcall(function() return C4:GetDeviceDisplayName(id) end)
  return (ok and type(n) == "string") and n or ""
end

-- A device id still resolving to a name is still in the project. Composer
-- deletions leave our persisted id dangling, and re-adding on a dangling id is
-- exactly the behaviour we want (the device really is gone).
local function deviceExists(deviceId)
  return displayName(deviceId) ~= ""
end

-- Devices in this project that came from a given .c4z. C4:GetDevicesByC4iName
-- takes the driver's internal .c4i name; for a DriverWorks .c4z that is the
-- base filename with a .c4i extension. Case handling differs between cores, so
-- try the obvious spellings and merge — this is a defensive best-effort, never
-- the primary source of truth.
local function devicesFromC4z(c4z)
  local raw = tostring(c4z or "")
  if raw == "" then return {} end
  local base = raw:gsub("%.c4z$", "")
  local found, seen = {}, {}
  -- C4:GetDevicesByC4iName matches the installed FILENAME. For a DriverWorks
  -- driver that is the ".c4z" name (verified on a live Director: ".c4i" returns
  -- nothing, ".c4z" returns every instance) — only stock drivers carry a ".c4i"
  -- filename. Try the c4z name first, then ".c4i" spellings so a stock-driver
  -- c4z value still resolves. Getting this wrong makes every reconcile see zero
  -- candidates and a resync duplicate every device.
  for _, candidate in ipairs({ raw, base .. ".c4z", base .. ".c4i", base:lower() .. ".c4i" }) do
    local ok, list = pcall(function() return C4:GetDevicesByC4iName(candidate) end)
    if ok and type(list) == "table" then
      for k, v in pairs(list) do
        -- Cores disagree on shape: id as value, id as key, or {id=..}.
        local n = tonumber(v) or tonumber(k) or (type(v) == "table" and tonumber(v.id or v.deviceid or v.deviceId))
        if n and not seen[n] then seen[n] = true; found[#found + 1] = n end
      end
    end
  end
  return found
end

--[[ Reconcile strategy — why there are TWO checks
     ------------------------------------------------------------------------
     Sync must be safe to press repeatedly; a duplicate keypad device in a
     project is a support call, not a cosmetic bug.

     1. PRIMARY: the persisted hardwareId -> deviceId map. Exact, survives
        driver reloads and Director restarts, and is the only check that can
        tell two identically-named devices apart.

     2. DEFENSIVE: if the map has no entry (driver was deleted and re-added,
        project restored, persist store wiped), fall back to scanning the
        project for a device created from the same .c4z whose display name
        matches the roster name. That is a heuristic — a dealer who renames a
        keypad in Composer defeats it — but it is strictly better than
        unconditionally re-adding, and any adoption it makes is written back
        into the map so the heuristic is needed at most once per device.

     A false NEGATIVE here duplicates a device (bad). A false POSITIVE adopts an
     existing device of the right type and name (harmless: the per-device driver
     re-identifies itself against the cloud anyway). So the heuristic is
     deliberately biased towards "assume it's already there".
--]]
-- Identity of an already-installed device: the per-device keypad driver
-- publishes its target MAC (== the NuVoxel hardwareId we handed it) as a
-- read-only "NV_HWID" variable. Reading it back reconciles a roster device to
-- its existing Control4 instance by identity — immune to renames and duplicate
-- names, and the primary recovery path when the persisted map was lost (agent
-- deleted & re-added, persist wiped). This is the real fix for resync dupes.
local HWID_VAR = "NV_HWID"
local function normHwid(s) return (tostring(s or ""):lower():gsub("[^0-9a-f]", "")) end
local function deviceHwid(deviceId)
  local id = tonumber(deviceId)
  if not id or id == 0 then return "" end
  local ok, t = pcall(function() return C4:GetDeviceVariables(id) end)
  if not ok or type(t) ~= "table" then return "" end
  for _, info in pairs(t) do
    if type(info) == "table" and tostring(info.name) == HWID_VAR then
      return normHwid(info.value)
    end
  end
  return ""
end

local function findExistingDevice(dev)
  -- 1. PRIMARY: the persisted hardwareId -> deviceId map (exact).
  local mapped = gDeviceMap[dev.hardwareId]
  if type(mapped) == "table" and deviceExists(mapped.id) then
    return tonumber(mapped.id), "map"
  end
  if type(mapped) == "table" then
    dbg("mapped device", tostring(mapped.id), "for", dev.hardwareId, "no longer in project — will re-adopt")
    gDeviceMap[dev.hardwareId] = nil
  end

  local present = devicesFromC4z(dev.driverC4z)

  -- Diagnostics (turn on Debug Logging): reveals WHY a resync duplicates on a real
  -- Director — whether the persisted map has this hwid, and exactly which devices
  -- GetDevicesByC4iName returns (do the intercom proxy children show up here?).
  dbg("findExisting hwid", dev.hardwareId, "want-name='" .. tostring(dev.name) .. "'",
      "map?", (type(gDeviceMap[dev.hardwareId]) == "table") and ("yes id=" .. tostring(gDeviceMap[dev.hardwareId].id)) or "no",
      "candidates=" .. tostring(#present))
  for _, id in ipairs(present) do
    dbg("   candidate id", id, "name='" .. displayName(id) .. "' hwid='" .. deviceHwid(id)
        .. "' exists=" .. tostring(deviceExists(id)))
  end

  -- 2. IDENTITY MATCH: a candidate whose published NV_HWID is this device. Exact,
  --    and immune to renames or several identically-named keypads. This is what
  --    stops a resync from duplicating after the map is lost.
  local wantHw = normHwid(dev.hardwareId)
  if wantHw ~= "" then
    for _, id in ipairs(present) do
      if deviceHwid(id) == wantHw then return id, "hwid-match" end
    end
  end

  -- 3. NAME MATCH: a device of this driver still named the roster name. Cheap and
  --    exact — until the dealer renames it in Composer. Kept as a fallback for
  --    instances that have no identity yet (never received a MAC / older build).
  local wanted = tostring(dev.name or "")
  if wanted ~= "" then
    for _, id in ipairs(present) do
      if displayName(id) == wanted then return id, "name-match" end
    end
  end

  -- 4. ORPHAN CLAIM: a device of this driver that NO map entry points to is an
  --    orphan (renamed in Composer, driver re-added, persist wiped). Claiming it
  --    beats adding a duplicate. Only when EXACTLY ONE is unclaimed, so several
  --    same-model keypads are never mis-assigned to each other — those fall back
  --    to name-match, and a genuinely new unit is added.
  local claimed = {}
  for _, e in pairs(gDeviceMap) do
    if type(e) == "table" and e.id then claimed[tonumber(e.id)] = true end
  end
  local orphan
  for _, id in ipairs(present) do
    if not claimed[id] then
      if orphan then orphan = nil; break end -- >1 unclaimed: ambiguous, don't guess
      orphan = id
    end
  end
  if orphan then return orphan, "orphan-claim" end

  return nil
end

-- Has the portal said where this device goes? The room must come from the
-- roster and nowhere else — there is no C4:RoomGetId() fallback, because an
-- agent has no room and guessing one is the bug this driver exists to fix.
-- An OLD server, which sends no installState at all, lands here as
-- "unassigned": nothing installs and Status says so.
local function isPlaced(dev)
  return dev.installState == "assigned" and dev.c4RoomId > 0
end

-- ============================================================================
-- Room list push
-- ----------------------------------------------------------------------------
-- Rooms are project items of type ROOM_DEVICE, i.e. devices running
-- roomdevice.c4i. UNVERIFIED on a live Director: that such items exist is
-- confirmed, that this call returns them is not — so every step degrades to
-- "no rooms", which costs the customer a picker, not a working system.
-- ============================================================================
local ROOM_C4I = "roomdevice.c4i"

-- GetDevicesByC4iName is documented to return device ids, but cores have been
-- seen returning tables. Accept either rather than lose the whole list to one
-- unexpected shape.
local function asDeviceId(v)
  if type(v) == "number" then return v end
  if type(v) == "string" then return tonumber(v) end
  if type(v) == "table" then
    return tonumber(v.id or v.deviceid or v.deviceId or v.device_id)
  end
  return nil
end

local function projectRooms()
  local rooms, seen = {}, {}
  local ok, list = pcall(function() return C4:GetDevicesByC4iName(ROOM_C4I) end)
  if not ok or type(list) ~= "table" then return rooms end
  -- Shape matters, and it was the bug: GetDevicesByC4iName returns a table keyed
  -- ROOMID -> ROOMNAME (confirmed by Snap One's own lib.lua:
  -- `for roomid, roomname in pairs(...)`). The old loop took the VALUE (the name
  -- string) as the entry and tried to parse it as an id, so tonumber("Kitchen")
  -- = nil dropped every room. Read the id from the KEY when the value is a name
  -- string; fall back to the array/id shape (value is the id) some cores use.
  for k, v in pairs(list) do
    local id, name
    if type(v) == "string" then
      id, name = asDeviceId(k), v            -- id -> name (this Director's shape)
    else
      id, name = asDeviceId(v), nil          -- index -> id (array shape)
    end
    if id and id > 0 and not seen[id] then
      seen[id] = true
      if not name or name == "" then name = displayName(id) end
      -- A room we can't name is a room the customer can't pick.
      if name ~= "" then rooms[#rooms + 1] = { c4RoomId = id, name = name } end
    end
  end
  return rooms
end

-- Fire-and-forget: the portal treats an empty or unreadable push as "keep what
-- you had", and a push failure must never disturb the poll that triggered it.
function PushRooms()
  local key = orgKey()
  if key == "" then return end
  local rooms = projectRooms()
  if #rooms == 0 then
    dbg("no rooms found via", ROOM_C4I, "— portal keeps its previous list")
    return
  end
  -- This driver's own device id: not literally the controller, but the same
  -- "which install spoke to us" identifier the keypad driver reports.
  local okId, myId = pcall(function() return C4:GetDeviceID() end)
  local body = json.encode({
    controllerId = tostring((okId and myId) or 0),
    rooms = rooms,
  })
  local ok = pcall(function()
    C4:url()
      :OnDone(function(_, _, errCode)
        dbg("pushed", #rooms, "room(s):", errCode == 0 and "ok" or ("failed " .. tostring(errCode)))
      end)
      :Post(CloudUrl() .. "/api/v1/org/c4/rooms", body,
            { ["Content-Type"] = "application/json", ["x-api-key"] = key })
  end)
  if not ok then dbg("room push: C4:url() unavailable") end
end

-- ============================================================================
-- Roster fetch
-- ============================================================================
-- Normalise one roster entry. Everything is optional and everything is coerced:
-- a new SKU may carry fields this build has never heard of, and an older server
-- may omit fields this build expects. Neither may throw.
local function normalise(d)
  if type(d) ~= "table" then return nil end
  local hwid = tostring(d.hardwareId or "")
  if hwid == "" then return nil end   -- without an identity we can't reconcile it at all
  return {
    hardwareId      = hwid,
    kind            = tostring(d.kind or ""),
    deviceSku       = tostring(d.deviceSku or ""),
    name            = tostring(d.name or ""),
    model           = tostring(d.model or d.deviceSku or ""),
    online          = d.online and true or false,
    firmwareVersion = tostring(d.firmwareVersion or ""),
    driverC4z       = tostring(d.driverC4z or ""),
    -- Companion intercom driver, non-empty ONLY when the device is licensed for
    -- intercom. We install + bind it to the keypad when set; remove it when it clears.
    intercomC4z     = tostring(d.intercomC4z or ""),
    -- Portal-owned placement. Absent -> "" -> unassigned, i.e. don't install.
    c4RoomId        = tonumber(d.c4RoomId) or 0,
    installState    = tostring(d.installState or ""),
    -- Device-reported LAN IP, used for the MAC->IP handoff (NV_SET_TARGET_IP) that
    -- binds the keypad driver without multicast SDDP. This normaliser is a strict
    -- whitelist, so omitting the field here silently disabled that whole path: the
    -- handoff's `dev.lanIp ~= ""` guard could never be true. Validated as dotted
    -- IPv4 so a malformed report can't reach C4:SetBindingAddress.
    lanIp           = tostring(d.lanIp or ""):match("^%s*(%d+%.%d+%.%d+%.%d+)%s*$") or "",
  }
end

-- Split the roster into what a Sync would do: how many installable devices are
-- missing from the project, and how many are stuck waiting on a room choice.
-- Both numbers are what the dealer needs to know whether to press Sync at all.
local function countWork(roster)
  local missing, awaiting = 0, 0
  for _, d in ipairs(roster or {}) do
    if d.driverC4z ~= "" and d.installState ~= "skip" and not findExistingDevice(d) then
      if isPlaced(d) then missing = missing + 1 else awaiting = awaiting + 1 end
    end
  end
  return missing, awaiting
end

-- Fetch the roster and hand it to cb(rosterOrNil). Every failure path sets a
-- human-readable Status itself, so callers only need the success case.
function FetchRoster(cb)
  local key = orgKey()
  if key == "" then
    setStatus("Not linked — approve in the browser, or paste an Organization Key")
    return cb(nil)
  end

  local url = CloudUrl() .. "/api/v1/org/roster"
  dbg("fetching roster from", url)
  local ok = pcall(function()
    C4:url()
      :OnDone(function(transfer, responses, errCode, errMsg)
        local resp = responses and responses[#responses]
        local code = resp and resp.code
        -- C4:url() uses fail-on-error, so an HTTP >= 400 comes back as a NON-ZERO
        -- errCode (22 = CURLE_HTTP_RETURNED_ERROR), not errCode 0 with a readable
        -- code. The roster is auth-gated, so a reached-but-errored call is almost
        -- always a missing/invalid key — NOT "cannot reach". Classify that (from
        -- the code if we got one, else errCode 22) before the transport case, so
        -- the installer sees an actionable message instead of a network scare.
        if code == 401 or code == 403 then
          setStatus("NuVoxel rejected the account link (" .. tostring(code) .. ") — re-link with 'Link to NuVoxel Account'")
          fireEvent("Sync Failed")
          return cb(nil)
        end
        if errCode == 22 then
          -- Reached the server, got an error status we couldn't read: could be a
          -- transient server error OR a rejected key. DON'T declare "not linked" —
          -- the stored key is intact (LinkStart would say "already linked"). Say
          -- sync failed and point at the re-link recovery.
          setStatus("Couldn't sync with " .. CloudUrl() .. " (server error) — will retry; re-link if it persists")
          fireEvent("Sync Failed")
          return cb(nil)
        end
        if errCode ~= 0 or not resp then
          -- Genuine transport failure: no DNS, no route, TLS, timeout.
          setStatus("Cannot reach " .. CloudUrl() .. " (" .. tostring(errMsg or errCode) .. ")")
          fireEvent("Sync Failed")
          return cb(nil)
        end
        if code ~= 200 then
          setStatus("Server error " .. tostring(code) .. " fetching roster")
          fireEvent("Sync Failed")
          return cb(nil)
        end
        local pok, body = pcall(json.decode, resp.body or "")
        if not pok or type(body) ~= "table" or type(body.devices) ~= "table" then
          setStatus("Unreadable roster response from server")
          fireEvent("Sync Failed")
          return cb(nil)
        end
        local roster = {}
        for _, d in ipairs(body.devices) do
          local n = normalise(d)
          if n then roster[#roster + 1] = n end
        end
        gRoster = roster
        dbg("roster:", #roster, "device(s)")
        -- Every successful poll also refreshes the portal's room list, so the
        -- picker a customer opens reflects the project as it is now. Done here
        -- rather than in the callers so refresh, sync and a key change all get
        -- it, and only ever after the key has proven good.
        PushRooms()
        return cb(roster)
      end)
      :Get(url, { ["x-api-key"] = key, ["Accept"] = "application/json" })
  end)
  if not ok then
    setStatus("C4:url() unavailable — Director OS too old?")
    cb(nil)
  end
end

-- ============================================================================
-- Refresh (look only — NEVER installs)
-- ============================================================================
function RefreshRoster()
  FetchRoster(function(roster)
    if not roster then return end          -- FetchRoster already set Status
    if #roster == 0 then
      setStatus("Account has no devices yet — claim a device in the NuVoxel app first")
      return
    end
    local missing, awaiting = countWork(roster)
    local waitMsg = awaiting > 0
      and ("; " .. awaiting .. " device(s) awaiting room assignment in the portal") or ""
    if missing == 0 and awaiting == 0 then
      -- Truly nothing outstanding: every account device is in the project.
      setStatus(#roster .. " device(s) in account, all installed")
    elseif missing == 0 then
      -- Nothing INSTALLABLE right now — the remaining devices still need a room
      -- picked in the portal. "all installed" was wrong here (installed count is 0).
      setStatus(#roster .. " device(s) in account" .. waitMsg)
    else
      setStatus(missing .. " new device(s) available — run \"Sync Devices From Account\"" .. waitMsg)
      fireEvent("New Devices Available")
    end
  end)
end

function StartRefreshTimer()
  cancelTimer(gRefreshTimer); gRefreshTimer = nil
  local ms = REFRESH_MS[tostring(Properties["Refresh Interval"] or "1 hour")] or 0
  if ms == 0 then dbg("refresh timer off"); return end
  gRefreshTimer = C4:SetTimer(ms, function() RefreshRoster() end, true)  -- repeating; ms
  dbg("refresh timer every", ms, "ms")
end

-- ── Communication agent re-enroll ────────────────────────────────────────────
-- After we install/bind intercomproxy endpoints, the Control4 Communication agent
-- must re-scan to enroll them into its SIP directory (xml_curl). Until it does, the
-- device REGISTERs but FreeSWITCH answers "Can't find user" — the endpoint is
-- provisioned but not enrolled. The agent exposes this exact re-scan as its own
-- "Sync Registered Devices" action (command SYNC_REGISTERED_DEVICES); we fire it a
-- few seconds after a sync so the freshly-added intercoms have inited and reported
-- their SIP username first. Idempotent — re-syncing already-enrolled endpoints is a
-- no-op. Filename match tries .c4z then .c4i (see [[mmkeypad-agent-dedup-and-linkloss]]).
local COMM_AGENT_C4Z = { "control4_communication_agent_v2.c4z", "control4_communication_agent_v2.c4i" }
local gCommSyncPending = false
local gCommSyncTimer   = nil
local function nudgeCommAgentNow()
  local seen, hit = {}, false
  for _, nm in ipairs(COMM_AGENT_C4Z) do
    local ok, devs = pcall(function() return C4:GetDevicesByC4iName(nm) end)
    if ok and type(devs) == "table" then
      for id in pairs(devs) do
        if not seen[id] then
          seen[id] = true; hit = true
          pcall(function() C4:SendToDevice(id, "SYNC_REGISTERED_DEVICES", {}) end)
          dbg("Communication agent", id, "SYNC_REGISTERED_DEVICES (enroll intercoms)")
        end
      end
    end
  end
  if not hit then dbg("Communication agent not found — intercom enroll skipped") end
end
-- Debounced: one nudge per sync that touched a licensed intercom, delayed so async
-- AddDevice + the intercom's LateInit provision land before the agent re-scans.
local function scheduleCommAgentSync()
  if not gCommSyncPending then return end
  gCommSyncPending = false
  cancelTimer(gCommSyncTimer)
  gCommSyncTimer = C4:SetTimer(8000, function() gCommSyncTimer = nil; nudgeCommAgentNow() end, false)
end

-- Reconcile an already-installed intercom to this keypad by IDENTITY: the intercom
-- publishes its room as a read-only NV_ROOM variable, and the one whose room == the
-- keypad's room is this keypad's intercom. Mirrors the keypad NV_HWID reconcile, so a
-- resync with a lost/stale map (agent re-added, persist wiped, or the mapped intercom
-- was deleted) adopts the existing intercom instead of installing a duplicate.
local function deviceRoomVar(deviceId)
  local id = tonumber(deviceId)
  if not id or id == 0 then return "" end
  local ok, t = pcall(function() return C4:GetDeviceVariables(id) end)
  if not ok or type(t) ~= "table" then return "" end
  for _, info in pairs(t) do
    if type(info) == "table" and tostring(info.name) == "NV_ROOM" then
      return tostring(info.value or "")
    end
  end
  return ""
end
local function findExistingIntercom(c4z, room)
  if not room or tonumber(room) == nil or tonumber(room) <= 0 then return nil end
  for _, id in ipairs(devicesFromC4z(c4z)) do
    if deviceExists(id) and deviceRoomVar(id) == tostring(room) then return id end
  end
  return nil
end

-- Install + bind the companion intercom driver for a keypad, gated on license.
-- The keypad's intercom is a SEPARATE intercomproxy-primary driver (only shape the
-- Communication agent enrolls); it reaches the device by relaying through the keypad
-- over a control binding. We AddDevice it in the keypad's room, then tell the KEYPAD
-- to bind the relay (C4:Bind must be called by one of the two parties, which the agent
-- is not). The installed id is remembered in gDeviceMap[hwid].intercomId so we bind
-- (not re-install) on every subsequent sync. dev.intercomC4z is "" unless licensed.
local function EnsureIntercom(dev, keypadId)
  local hwid = dev.hardwareId
  local entry = gDeviceMap[hwid]
  if type(entry) ~= "table" or not keypadId then return end
  local want   = dev.intercomC4z or ""
  local haveId = entry.intercomId and tonumber(entry.intercomId)

  if want == "" then
    -- Not licensed for intercom. Control4 gives a driver no DeleteDevice, so a lapsed
    -- license can't auto-remove the intercom item — surface it for the dealer instead.
    if haveId and deviceExists(haveId) then
      dbg("intercom license lapsed for", hwid, "— remove device", haveId, "in Composer")
      setStatus("Intercom license lapsed on a device — remove its Intercom in Composer")
    end
    return
  end

  -- This device is licensed for intercom (want ~= ""), so the sync should nudge the
  -- Communication agent to (re)enroll — covers both a fresh install below and the
  -- recovery case where an already-installed endpoint isn't enrolled yet.
  gCommSyncPending = true

  -- Already installed and still in the project → just (re)bind, idempotently.
  if haveId and deviceExists(haveId) then
    pcall(function() C4:SendToDevice(keypadId, "BindIntercom", { deviceId = haveId }, false) end)
    return
  end

  local room = dev.c4RoomId
  if not room or room <= 0 then return end

  -- Map miss (or the mapped id was deleted): adopt an existing intercom in this
  -- keypad's room before installing, so a resync never duplicates. Write it back to
  -- the map so this identity scan is needed at most once per device.
  local adopt = findExistingIntercom(want, room)
  if adopt then
    entry.intercomId = adopt; saveMap()
    pcall(function() C4:SendToDevice(keypadId, "BindIntercom", { deviceId = adopt }, false) end)
    dbg("adopted existing intercom", adopt, "for", hwid, "in room", room, "(NV_ROOM match)")
    return
  end
  local label = (dev.name ~= "" and dev.name or "Keypad") .. " Intercom"
  dbg("installing intercom", want, "for", hwid, "in room", room)
  pcall(function()
    C4:AddDevice(want, room, label, function(icId)
      local id = tonumber(icId) or 0
      if id == 0 then dbg("intercom AddDevice failed for", hwid); return end
      local e = gDeviceMap[hwid]
      if type(e) == "table" then e.intercomId = id; saveMap() end
      dbg("intercom installed", id, "for", hwid, "— binding to keypad", keypadId)
      -- The keypad owns the :6700 link and does the C4:Bind (it is a party to it).
      pcall(function() C4:SendToDevice(keypadId, "BindIntercom", { deviceId = id }, false) end)
    end)
  end)
end

-- ============================================================================
-- Sync (installs) — only ever reached from the Composer Action
-- ============================================================================
function SyncDevices()
  if gSyncing then
    setStatus("Sync already in progress")
    return
  end
  gSyncing = true
  FetchRoster(function(roster)
    if not roster then gSyncing = false; return end
    if #roster == 0 then
      gSyncing = false
      setStatus("Account has no devices yet — claim a device in the NuVoxel app first")
      return
    end

    -- `pending` starts at 1 as a guard: an AddDevice callback that fires
    -- synchronously would otherwise drive the count to 0 mid-loop and report a
    -- "finished" sync while devices are still being added. The guard is
    -- released after the loop.
    local pending, added, existing, skipped, failed, awaiting = 1, 0, 0, 0, 0, 0
    local renamed, orphaned = 0, 0
    local seen = {}   -- guards against a roster that lists the same hardwareId twice

    -- Reported once all AddDevice callbacks have come back (or immediately if
    -- none were needed). Everything the dealer needs is in one line.
    local function finish()
      gSyncing = false
      saveMap()
      updateCountProperty()
      local parts = {}
      if added    > 0 then parts[#parts + 1] = added .. " installed" end
      if existing > 0 then parts[#parts + 1] = existing .. " already present" end
      if renamed  > 0 then parts[#parts + 1] = renamed .. " renamed" end
      if skipped  > 0 then parts[#parts + 1] = skipped .. " not installable" end
      if failed   > 0 then parts[#parts + 1] = failed .. " failed" end
      if #parts == 0 then parts[#parts + 1] = "nothing to do" end
      -- Kept out of the comma list and spelled out in full: it is the one
      -- outcome the dealer can act on, and the action is not in Composer.
      local waitMsg = awaiting > 0
        and ("; " .. awaiting .. " device(s) awaiting room assignment in the portal") or ""
      local orphanMsg = orphaned > 0
        and ("; " .. orphaned .. " removed in the portal — delete in Composer (Control4 has no delete API for drivers)") or ""
      setStatus("Synced " .. #roster .. " account device(s): " .. table.concat(parts, ", ") .. waitMsg .. orphanMsg)
      if orphaned > 0 then fireEvent("Device Removed") end
      if added > 0 then fireEvent("Devices Installed") end
      if failed > 0 then fireEvent("Sync Failed") end
      -- Enroll any (newly) installed intercoms into the Communication agent's SIP
      -- directory, so a licensed panel can call door stations/other panels — not just
      -- REGISTER and get "Can't find user". Debounced + delayed inside.
      scheduleCommAgentSync()
    end

    for _, dev in ipairs(roster) do
      if seen[dev.hardwareId] then
        -- duplicate row; already handled
      elseif dev.driverC4z == "" then
        -- The server says this device has no Control4 driver (a phone, a
        -- sensor, a SKU that isn't a C4 device). Not an error.
        seen[dev.hardwareId] = true
        skipped = skipped + 1
        dbg("skip", dev.hardwareId, "(" .. dev.deviceSku .. "): no driverC4z")
      elseif dev.installState == "skip" then
        -- The customer said this one isn't a Control4 device. Also not an error,
        -- and pointedly not "awaiting" anything.
        seen[dev.hardwareId] = true
        skipped = skipped + 1
        dbg("skip", dev.hardwareId, ": excluded from Control4 in the portal")
      elseif not isPlaced(dev) and not findExistingDevice(dev) then
        -- No room chosen. Installing anyway is the old bug; the customer
        -- finishes this in the portal, so all we can do is say so.
        seen[dev.hardwareId] = true
        awaiting = awaiting + 1
        dbg("awaiting room assignment:", dev.hardwareId, "(" .. dev.name .. ")")
      else
        seen[dev.hardwareId] = true
        local id, how = findExistingDevice(dev)
        if id then
          existing = existing + 1
          -- Portal rename -> Control4 rename. RenameDevice is the ONLY project
          -- mutation Control4 exposes to a driver (there is no DeleteDevice API),
          -- and it may be called on another driver's device. Compare first so we
          -- don't churn the project on every sync.
          if dev.name ~= "" then
            local okN, cur = pcall(function() return C4:GetDeviceDisplayName(id) end)
            if okN and cur ~= dev.name then
              pcall(function() C4:RenameDevice(id, dev.name) end)
              renamed = renamed + 1
              dbg("renamed", dev.hardwareId, "device", id, "->", dev.name)
            end
          end
          -- Adopt into the map even when it came from the map, so c4z/name stay current.
          local prevIc = (type(gDeviceMap[dev.hardwareId]) == "table") and gDeviceMap[dev.hardwareId].intercomId or nil
          gDeviceMap[dev.hardwareId] = { id = id, c4z = dev.driverC4z, name = dev.name, intercomId = prevIc }
          dbg("present", dev.hardwareId, "-> device", id, "(" .. how .. ")")
          EnsureIntercom(dev, id)   -- install + bind the companion intercom if licensed
          -- MAC->IP handoff: hand the keypad its device's current LAN IP (from the cloud
          -- roster; the device reports it on check-in) so it binds directly — bypassing
          -- flaky multicast SDDP. Runs every sync so a DHCP move re-binds. Idempotent
          -- driver-side (no-op when already connected there); SDDP + hello stay the fallback.
          if type(dev.lanIp) == "string" and dev.lanIp ~= "" then
            pcall(function() C4:SendToDevice(id, "NV_SET_TARGET_IP", { ip = dev.lanIp }, false) end)
          end
        else
          local label = dev.name ~= "" and dev.name or (dev.model ~= "" and dev.model or dev.deviceSku)
          -- Per device, not per sync: this is the whole fix. The room is the
          -- portal's answer or nothing at all. isPlaced() has already vouched
          -- for it; this re-check is here so that a future edit which loosens
          -- isPlaced() cannot quietly resurrect "install into room 0", which
          -- Director would either reject or dump the device into limbo.
          local room = dev.c4RoomId
          if room <= 0 then
            failed = failed + 1
            dbg("refusing to add", dev.hardwareId, "— roster gave no room")
          else
            dbg("adding", dev.driverC4z, "for", dev.hardwareId, "as", label, "in room", room)
            pending = pending + 1
            local hwid = dev.hardwareId
            local c4z  = dev.driverC4z
            local okAdd, ret = pcall(function()
              -- Callback gets (deviceId, tDeviceInfo); tDeviceInfo carries the
              -- other proxy ids for a multi-proxy driver. We persist the id we
              -- are handed, which is the one C4:GetDeviceDisplayName resolves.
              return C4:AddDevice(c4z, room, label, function(deviceId, tDeviceInfo)
                local newId = tonumber(deviceId) or 0
                if newId == 0 then
                  failed = failed + 1
                  dbg("AddDevice callback reported failure for", hwid)
                else
                  added = added + 1
                  gDeviceMap[hwid] = { id = newId, c4z = c4z, name = label }
                  dbg("added", hwid, "-> device", newId)
                  -- Hand the new keypad driver its own device's MAC so it can find
                  -- and bind to it on the LAN via SDDP (AddDevice leaves the network
                  -- binding empty). hwid IS the MAC. The keypad driver also learns
                  -- this from its first hello, so this only accelerates the very
                  -- first bind. Best-effort; a delivery miss just falls back to the
                  -- device's own hello or the manual override.
                  pcall(function()
                    C4:SendToDevice(newId, "NV_SET_TARGET_MAC", { mac = hwid }, false)
                  end)
                  EnsureIntercom(dev, newId)   -- install + bind companion intercom if licensed
                end
                pending = pending - 1
                if pending == 0 then finish() end
              end)
            end)
            -- AddDevice returns 0 on failure — in that case no callback will ever
            -- fire, so decrement here or finish() would never be reached. An agent
            -- being barred from AddDevice outright (see the header note) would
            -- surface here, as a "failed" count the dealer can see.
            if not okAdd or ret == 0 then
              failed = failed + 1
              pending = pending - 1
              dbg("AddDevice failed immediately for", hwid, "(" .. c4z .. ")",
                  "— is the .c4z in the Director driver database?")
              -- Most likely the Director doesn't have this (possibly brand-new)
              -- driver yet. Install it from the catalog once per load; the next
              -- Sync then adds the device. Bounded so a genuinely-barred AddDevice
              -- (agent restriction) doesn't loop.
              if not gDriverInstallTried[c4z] then
                gDriverInstallTried[c4z] = true
                EnsureDriverInstalled(c4z)
              end
            end
          end
        end
      end
    end

    -- Devices we still track that the portal no longer lists were removed there.
    -- Control4 gives a driver no DeleteDevice API, so we stop tracking the orphan
    -- and tell the dealer to delete it in Composer — we can't do it from here.
    -- Only entries absent from THIS roster are orphans; anything the roster listed
    -- (including skip / awaiting) is in `seen`.
    for hwid, ent in pairs(gDeviceMap) do
      if not seen[hwid] then
        orphaned = orphaned + 1
        dbg("orphan (removed in portal):", hwid, ent and ent.name or "?", "device", ent and ent.id or "?")
        gDeviceMap[hwid] = nil
      end
    end

    pending = pending - 1          -- release the guard
    if pending == 0 then finish() end
  end)
end

-- ============================================================================
-- Account linking — OAuth 2.0 Device Authorization Grant (RFC 8628)
-- ----------------------------------------------------------------------------
-- The driver has no browser and no session, which is exactly what device flow
-- is for: POST /link/start to get a short user code + a secret device code, show
-- the user code in Composer, and poll /link/poll until a human approves in the
-- portal and the server hands back a minted org key. The key is persisted (never
-- shown), after which the driver proceeds exactly as it does with a pasted key.
-- ============================================================================

-- Tear down the poll loop and forget the in-flight secret. Safe to call twice.
local function stopLinking()
  cancelTimer(gLinkTimer); gLinkTimer = nil
  gLinkDeviceCode = nil
  gLinkPolling = false
end

-- Repeating poll. Every non-terminal outcome just keeps the timer running; only
-- linked/consumed/expired stop it. A transport hiccup is transient — keep going.
function LinkPoll()
  if not gLinkDeviceCode then stopLinking(); return end
  if gLinkPolling then return end          -- previous poll still in flight
  gLinkPolling = true
  local body = json.encode({ deviceCode = gLinkDeviceCode })
  local ok = pcall(function()
    C4:url()
      :OnDone(function(transfer, responses, errCode, errMsg)
        gLinkPolling = false
        local resp = responses and responses[#responses]
        if errCode ~= 0 or not resp then
          dbg("link poll transport error:", tostring(errMsg or errCode)); return
        end
        if resp.code ~= 200 then dbg("link poll http", tostring(resp.code)); return end
        local pok, data = pcall(json.decode, resp.body or "")
        if not pok or type(data) ~= "table" then return end
        local status = tostring(data.status or "")
        if status == "pending" then
          -- keep waiting; Status already shows the code + URL
        elseif status == "slow_down" then
          -- polled too fast; back off and reschedule the repeating timer
          gLinkIntervalMs = math.min(30000, (gLinkIntervalMs or 5000) + 2000)
          cancelTimer(gLinkTimer)
          gLinkTimer = C4:SetTimer(gLinkIntervalMs, function() LinkPoll() end, true)
        elseif status == "linked" then
          local key = tostring(data.apiKey or "")
          if key == "" then
            stopLinking()
            setStatus("Link approved but no key returned — run \"Link to NuVoxel Account\" again")
            return
          end
          -- The key is handed over exactly once; persist it before anything else
          -- can fail, then behave exactly as a pasted key would.
          persistSet(PERSIST_KEY, key)
          local orgName = tostring(data.orgName or "")
          stopLinking()
          pcall(function() C4:UpdateProperty("Link Code", "") end)
          setStatus("Linked to " .. (orgName ~= "" and orgName or "your NuVoxel account"))
          fireEvent("Linked")
          RefreshRoster()            -- look, don't install (same rule as a reload)
        elseif status == "consumed" then
          stopLinking()
          pcall(function() C4:UpdateProperty("Link Code", "") end)
          setStatus("Link already completed")
        else   -- "expired" or anything unrecognised: give up, let the user retry
          stopLinking()
          pcall(function() C4:UpdateProperty("Link Code", "") end)
          setStatus("Link code expired — run \"Link to NuVoxel Account\" again")
        end
      end)
      :Post(CloudUrl() .. "/api/v1/driver/link/poll", body,
            { ["Content-Type"] = "application/json", ["Accept"] = "application/json" })
  end)
  if not ok then gLinkPolling = false; dbg("link poll: C4:url() unavailable") end
end

-- Begin (or restart) linking. No-op with a message if a key is already in hand —
-- re-linking requires an explicit Unlink first so a working system is never
-- disturbed by a stray Action press.
function LinkStart()
  if orgKey() ~= "" then
    setStatus("Already linked — use \"Unlink NuVoxel Account\" first to re-link")
    return
  end
  stopLinking()
  local body = json.encode({ instanceId = instanceId() })
  local ok = pcall(function()
    C4:url()
      :OnDone(function(transfer, responses, errCode, errMsg)
        local resp = responses and responses[#responses]
        if errCode ~= 0 or not resp then
          setStatus("Cannot reach " .. CloudUrl() .. " to start linking (" .. tostring(errMsg or errCode) .. ")")
          fireEvent("Sync Failed")
          return
        end
        if resp.code ~= 200 then
          setStatus("Server error " .. tostring(resp.code) .. " starting link")
          fireEvent("Sync Failed")
          return
        end
        local pok, data = pcall(json.decode, resp.body or "")
        if not pok or type(data) ~= "table" or not data.deviceCode or not data.userCode then
          setStatus("Unreadable link response from server")
          return
        end
        gLinkDeviceCode = tostring(data.deviceCode)
        local userCode  = tostring(data.userCode)
        local verifyUrl = tostring(data.verificationUrl)
        if verifyUrl == "" or verifyUrl == "nil" then verifyUrl = CloudUrl() .. "/link" end
        local intervalSec = tonumber(data.intervalSec) or 5
        gLinkIntervalMs = math.max(2000, intervalSec * 1000)
        pcall(function() C4:UpdateProperty("Link Code", userCode) end)
        pcall(function() C4:UpdateProperty("Link At", verifyUrl) end)
        setStatus("Go to " .. verifyUrl .. " and enter " .. userCode)
        fireEvent("Link Pending")
        gLinkTimer = C4:SetTimer(gLinkIntervalMs, function() LinkPoll() end, true)  -- repeating; ms
      end)
      :Post(CloudUrl() .. "/api/v1/driver/link/start", body,
            { ["Content-Type"] = "application/json", ["Accept"] = "application/json" })
  end)
  if not ok then setStatus("C4:url() unavailable — Director OS too old?") end
end

-- Drop the linked key and start over. The pasted property (if any) is untouched;
-- clearing a persisted, browser-approved key is the only thing this does.
function Unlink()
  stopLinking()
  persistSet(PERSIST_KEY, "")
  pcall(function() C4:UpdateProperty("Link Code", "") end)
  if orgKey() ~= "" then
    -- A pasted Organization Key is still in play — nothing to link.
    setStatus("Cleared linked key; still using the pasted Organization Key")
    return
  end
  setStatus("Unlinked — starting a new link")
  LinkStart()
end

-- ============================================================================
-- Lifecycle
-- ============================================================================
-- ── Driver self-update (mirrors Chowmain's Project Agent) ────────────────────
-- This agent keeps EVERY NuVoxel Control4 driver current — including itself. It
-- asks the catalog which drivers we publish (GET /api/v1/fw/releases?kind=
-- c4-driver), and for each one actually installed in the project, downloads a
-- newer build and — using the proven community mechanism (finitelabs / black-ops
-- / frigate) — writes it to the c4z store ROOT and drives Composer's local SOAP
-- endpoint (127.0.0.1:5020, UpdateProjectC4i) to install it; the controller then
-- reloads the updated driver. Publish a new driver and it's picked up here with
-- no client change.
--
-- NOTE: the FileSetDir / C4Z_ROOT / UpdateProjectC4i install path is OS-version
-- sensitive and has NOT been validated on a live Director in this build. Default
-- is Automatic Updates = Off; test "Install Driver Updates Now" on one Director
-- before enabling it fleet-wide.
local SELF_C4Z       = "NuVoxelAgent.c4z"
local PERSIST_FWVER  = "nv_installed_fw_versions"
local gUpdateTimer   = nil

local function updateChannel()
  local c = Properties and tostring(Properties["Update Channel"] or "") or ""
  return (c ~= "" and c ~= "nil") and c or "stable"
end
local function autoUpdatesOn()
  return (Properties and tostring(Properties["Automatic Updates"] or "Off")) == "On"
end
local function setUpdStatus(s)
  dbg("update:", s)
  pcall(function() C4:UpdateProperty("Update Status", tostring(s) .. "  (" .. now() .. ")") end)
end

-- YYYY.MM.DD.NNN(+tag) -> one comparable integer (NNN up to 999).
local function verKey(v)
  local y, m, d, n = tostring(v or ""):match("(%d+)%.(%d+)%.(%d+)%.(%d+)")
  if not y then return 0 end
  return (((tonumber(y) * 100 + tonumber(m)) * 100 + tonumber(d)) * 1000) + tonumber(n)
end

local function loadInstalledVers()
  local raw = ""
  pcall(function() raw = C4:PersistGetValue(PERSIST_FWVER) end)
  local ok, t = pcall(json.decode, raw or "")
  return (ok and type(t) == "table") and t or {}
end
local function saveInstalledVers(t)
  local ok, raw = pcall(json.encode, t)
  if ok then pcall(function() C4:PersistSetValue(PERSIST_FWVER, raw) end) end
end

-- Best-known running version of a managed driver. We know our OWN from the
-- build-stamped DRIVER_VERSION; for the others we track the last version we
-- installed (seeded on first sight, assuming the installed build == current
-- channel, so we don't needlessly reinstall).
local function runningVersion(c4z)
  if c4z == SELF_C4Z then return DRIVER_VERSION end
  return loadInstalledVers()[c4z]
end

local function isInstalled(c4z)
  local ok, list = pcall(function() return C4:GetDevicesByC4iName(c4z) end)
  return ok and list ~= nil and next(list) ~= nil
end

-- Write bytes to C4Z_ROOT (verified) and install by name via Composer's SOAP.
local function installC4z(c4z, bytes)
  -- UpdateProjectC4i reads C4Z_ROOT BY NAME, so the file must land there — NOT
  -- the per-driver C4Z subfolder (writing there is the classic silent no-op).
  local okDir = pcall(function() C4:FileSetDir("C4Z_ROOT") end)
  if not okDir then
    setUpdStatus("Cannot write " .. c4z .. " — C4Z_ROOT locked by Director OS; self-update unavailable")
    return false
  end
  -- Control4's raw file API is HANDLE-based: FileOpen -> FileSetPos ->
  -- FileWrite(handle, len, content) -> FileClose (see drivers-common-public
  -- lib.lua). The filename form C4:FileWrite(name, bytes, true) throws ("Write
  -- failed"). Delete any old copy first so the write starts clean at position 0.
  if C4:FileExists(c4z) then pcall(function() C4:FileDelete(c4z) end) end
  local okW = pcall(function()
    local fh = C4:FileOpen(c4z)
    C4:FileSetPos(fh, 0)
    C4:FileWrite(fh, #bytes, bytes)
    C4:FileClose(fh)
  end)
  if not okW then setUpdStatus("Write failed for " .. c4z); return false end
  -- FileWrite's return is unreliable (the common lib shadows it); verify on-disk.
  local size = nil
  pcall(function()
    if C4:FileExists(c4z) then
      local f = C4:FileOpen(c4z)
      if f and f ~= -1 then size = C4:FileGetSize(f); C4:FileClose(f) end
    end
  end)
  if size ~= #bytes then
    setUpdStatus("Write incomplete for " .. c4z .. " (" .. tostring(size) .. "/" .. #bytes .. " bytes)")
    return false
  end
  local soap = '<c4soap name="UpdateProjectC4i" session="0" operation="RWX" category="composer" async="0">'
    .. '<param name="name" type="string">' .. c4z .. '</param></c4soap>\0'
  local okT = pcall(function()
    C4:CreateTCPClient()
      :OnConnect(function(client) client:Write(soap); client:Close() end)
      :OnError(function(client, ec, em) pcall(function() client:Close() end); dbg("install TCP error", ec, em) end)
      :Connect("127.0.0.1", 5020)
  end)
  if not okT then setUpdStatus("Could not reach Composer service to install " .. c4z); return false end
  return true
end

local function downloadAndInstall(c4z, url, newVer)
  local ok = pcall(function()
    C4:url()
      :OnDone(function(transfer, responses, errCode, errMsg)
        local resp = responses and responses[#responses]
        if errCode ~= 0 or not resp or resp.code ~= 200 or not resp.body or #resp.body < 1 then
          setUpdStatus("Download failed for " .. c4z .. " (" .. tostring(errMsg or (resp and resp.code) or errCode) .. ")")
          return
        end
        if installC4z(c4z, resp.body) then
          local t = loadInstalledVers(); t[c4z] = newVer; saveInstalledVers(t)
          setUpdStatus("Installing " .. c4z .. " " .. newVer .. " — Director will reload the driver")
        end
      end)
      :Get(url, {})
  end)
  if not ok then setUpdStatus("C4:url() unavailable — Director OS too old for self-update") end
end

-- Install (or refresh) a driver's .c4z from the catalog so it exists in the
-- Director's driver database. Needed before AddDevice can instantiate a device of
-- a brand-new type we've released: the agent installs the driver here, then the
-- next Sync adds the device. Global (called from the SyncDevices AddDevice path,
-- which is defined earlier in the file). Async; two-step by design to avoid
-- racing the Director's post-install driver rescan.
function EnsureDriverInstalled(c4z)
  local url = CloudUrl() .. "/api/v1/fw/release?target=" .. c4z .. "&channel=" .. updateChannel()
  local ok = pcall(function()
    C4:url()
      :OnDone(function(transfer, responses, errCode, errMsg)
        local resp = responses and responses[#responses]
        if errCode ~= 0 or not resp or resp.code ~= 200 then
          setUpdStatus("Could not fetch driver " .. c4z .. " to install (" .. tostring((resp and resp.code) or errMsg or errCode) .. ")")
          return
        end
        local pok, body = pcall(json.decode, resp.body or "")
        if not pok or type(body) ~= "table" or not body.url then
          setUpdStatus("No catalog entry for driver " .. c4z)
          return
        end
        setUpdStatus("Installing driver " .. c4z .. " " .. tostring(body.version) .. " — re-run Sync to add the device")
        downloadAndInstall(c4z, body.url, body.version)
      end)
      :Get(url, { ["Accept"] = "application/json" })
  end)
  if not ok then setUpdStatus("C4:url() unavailable — cannot install driver " .. c4z) end
end

-- Apply one catalog list entry { target, version, url } to an installed driver.
-- Returns true if an update was available (and, when install=true, started).
local function applyEntry(entry, install)
  local c4z = entry.target
  local running = runningVersion(c4z)
  if running == nil then
    -- First sight of a managed driver we didn't install ourselves: assume the
    -- installed build == current channel and record it, so we don't reinstall.
    -- A genuinely older install is caught once a newer version publishes.
    local t = loadInstalledVers(); t[c4z] = entry.version; saveInstalledVers(t)
    dbg(c4z, "tracked at", entry.version)
    return false
  end
  if verKey(entry.version) > verKey(running) then
    if install then
      setUpdStatus("Updating " .. c4z .. " " .. running .. " -> " .. entry.version)
      downloadAndInstall(c4z, entry.url, entry.version)
    else
      setUpdStatus("Update available for " .. c4z .. ": " .. entry.version .. " (current " .. running .. ")")
    end
    return true
  end
  return false
end

-- Discover EVERY Control4 driver we publish on the update channel and update the
-- ones actually installed here (this agent included). install=false -> report
-- only. Global: called from ExecuteCommand / the timer / property changes.
function CheckAllDriverUpdates(install)
  local url = CloudUrl() .. "/api/v1/fw/releases?kind=c4-driver&channel=" .. updateChannel()
  local ok = pcall(function()
    C4:url()
      :OnDone(function(transfer, responses, errCode, errMsg)
        local resp = responses and responses[#responses]
        if errCode ~= 0 or not resp then setUpdStatus("Cannot reach " .. CloudUrl()); return end
        if resp.code ~= 200 then dbg("driver list http", tostring(resp.code)); return end
        local pok, body = pcall(json.decode, resp.body or "")
        local releases = (pok and type(body) == "table") and body.releases or nil
        if type(releases) ~= "table" then setUpdStatus("Unreadable release list from server"); return end
        local installed, pending = 0, 0
        for _, e in ipairs(releases) do
          if type(e) == "table" and e.target and e.version and isInstalled(e.target) then
            installed = installed + 1
            if applyEntry(e, install) then pending = pending + 1 end
          end
        end
        if installed == 0 then
          setUpdStatus("No NuVoxel drivers installed to update")
        elseif pending == 0 and not install then
          setUpdStatus("All " .. installed .. " installed NuVoxel driver(s) up to date")
        end
      end)
      :Get(url, { ["Accept"] = "application/json" })
  end)
  if not ok then setUpdStatus("C4:url() unavailable — Director OS too old?") end
end

function StartUpdateTimer()
  cancelTimer(gUpdateTimer); gUpdateTimer = nil
  if not autoUpdatesOn() then dbg("automatic driver updates off"); return end
  gUpdateTimer = C4:SetTimer(24 * 60 * 60 * 1000, function() CheckAllDriverUpdates(true) end, true)
  dbg("automatic driver updates on (" .. updateChannel() .. " channel)")
end

function OnDriverInit(initType)
  print("NuVoxelAgent: OnDriverInit (" .. tostring(initType) .. ")")
  C4:UpdateProperty("Driver Version", DRIVER_VERSION)
  -- NOTE: we deliberately do NOT call C4:AllowExecute(true). It unlocks arbitrary
  -- Lua execution against this driver's live state (Composer's Lua Input window
  -- and headless eval) — i.e. anyone who reaches the Director can read our stored
  -- account key and org secrets out of driver memory. It defaults to false;
  -- leave it that way in shipped builds. For a dev diagnostic session, add the
  -- call locally and rebuild — never in a release.
end

function OnDriverLateInit(initType)
  print("NuVoxelAgent: OnDriverLateInit")
  -- Re-unlock C4Z_ROOT writes for the self-updater. OS 3.3.0+ restricts FileSetDir
  -- for unsigned drivers; this community-standard handshake re-enables root access
  -- for the rest of this load. Harmless where the restriction isn't present. Must
  -- precede any C4Z_ROOT file op (see installC4z).
  pcall(function() C4:FileSetDir("c29tZXNwZWNpYWxrZXk=++11") end)
  gDeviceMap = loadMap()
  updateCountProperty()
  if orgKey() == "" then
    -- No key by either path. Device flow is the primary way to get one, so begin
    -- it automatically — the dealer sees a code to approve instead of a demand to
    -- paste a key. Pasting an Organization Key still works and short-circuits it.
    setStatus("Not linked — approve in the browser, or paste an Organization Key")
    LinkStart()
  else
    -- Look, don't touch: a fresh reload must never install anything (see the
    -- AddDevice note at the top of this file).
    RefreshRoster()
  end
  StartRefreshTimer()
  -- Driver self-update: start the (opt-in) auto timer and do one report-only
  -- check so "Update Status" reflects availability without a button press. This
  -- is async (C4:url) and never installs.
  StartUpdateTimer()
  CheckAllDriverUpdates(false)
end

-- Called before the driver is reloaded (DIT_UPDATING) or removed/shutdown.
-- Tear down every timer so nothing fires into a half-reloaded driver.
function OnDriverDestroyed(initType)
  dbg("OnDriverDestroyed (" .. tostring(initType) .. ")")
  cancelTimer(gRefreshTimer); gRefreshTimer = nil
  cancelTimer(gUpdateTimer); gUpdateTimer = nil
  stopLinking()          -- kill the poll loop too, so nothing fires post-reload
  gSyncing = false
end

function OnPropertyChanged(prop)
  dbg("Property changed:", prop)
  if prop == "Organization Key" or prop == "Cloud URL" then
    if orgKey() == "" then
      -- Both the pasted key and any linked key are now empty — offer linking.
      setStatus("Not linked — approve in the browser, or paste an Organization Key")
      LinkStart()
    else
      -- A key just became available (pasted, or Cloud URL changed with a key in
      -- hand): stop any in-flight link and validate immediately. Never installs.
      stopLinking()
      RefreshRoster()
    end
  elseif prop == "Refresh Interval" then
    StartRefreshTimer()
  elseif prop == "Automatic Updates" or prop == "Update Channel" then
    StartUpdateTimer()
    CheckAllDriverUpdates(false)   -- refresh the status for the new setting/channel
  end
end

function ExecuteCommand(cmd, params)
  -- Composer Actions arrive as LUA_ACTION with the <command> in params.ACTION —
  -- normalise or the whole Actions tab silently no-ops.
  if cmd == "LUA_ACTION" and params and params.ACTION then
    cmd = tostring(params.ACTION); params.ACTION = nil
  end
  dbg("ExecuteCommand:", cmd)
  if cmd == "SyncDevices" then
    SyncDevices()
  elseif cmd == "LinkAccount" then
    -- Explicit re-link. Drop any stored key first so LinkStart can't refuse with
    -- "already linked" — this is the recovery when a key survived a driver update
    -- but the server no longer accepts it (Sync shows rejected). Pressing this IS
    -- the intent to link fresh.
    persistSet(PERSIST_KEY, "")
    stopLinking()
    LinkStart()
  elseif cmd == "Unlink" then
    Unlink()
  elseif cmd == "RefreshRoster" then
    RefreshRoster()
  elseif cmd == "CheckDriverUpdates" then
    setUpdStatus("Checking for driver updates...")
    CheckAllDriverUpdates(false)
  elseif cmd == "InstallDriverUpdates" then
    setUpdStatus("Installing available driver updates...")
    CheckAllDriverUpdates(true)
  elseif cmd == "PushRooms" then
    -- The push rides along with every roster poll; this is for the case where a
    -- dealer has just added rooms and doesn't want to wait for the next one.
    local rooms = projectRooms()
    PushRooms()
    setStatus(#rooms == 0
      and "No Control4 rooms found in this project"
      or ("Sent " .. #rooms .. " room(s) to the portal"))
  elseif cmd == "ForgetMap" then
    -- Support escape hatch: clears our record of what we installed. The next
    -- Sync then relies purely on the name-match heuristic, so use it only after
    -- the dealer has deleted the devices in Composer.
    gDeviceMap = {}
    saveMap()
    updateCountProperty()
    setStatus("Installed-device map cleared")
  end
  -- Unknown commands are ignored on purpose (forward-compat).
end
