--[[ ===========================================================================
  NuVoxel Keypad — Control4 DriverWorks driver (per-device)
  ---------------------------------------------------------------------------
  Ships as NuVoxelKeypad.c4z. It is NOT an update to MediaKeypad.c4z and must never
  be built under that name: it declares a different <proxy> set (it absorbs the old
  companion intercom driver as sub-proxy 5003), and Control4 only instantiates
  proxies when a driver is first ADDED to a project — changing them under an
  installed driver corrupts the project. A never-installed filename has nothing to
  corrupt, which is the whole reason for the rename.

  Three proxies, dispatched by binding id in ReceivedFromProxy:
    5002 keypad (PRIMARY)  — programmable on-screen buttons + LEDs
    5003 intercomproxy     — SIP intercom endpoint (see intercom.lua)

  Settings are owned by the driver's Composer properties (local; there is no
  online service). An edit is applied straight to the device over the :6700
  protocol.

  Each keypad is an independent IP device. The DEVICE listens (TCP server);
  THIS driver connects OUT to it via a network binding (6001), so the installer
  binds the discovered device on the Connections page and the binding goes green.
  One driver instance == one device, in the room the instance is placed in.

  Wire protocol: ../PROTOCOL.md  (newline-delimited JSON)

  Control4 API notes (verified from local SDK docs, 32_serial_network_device):
    * C4:NetConnect(idBinding, nPort, "TCP")        -- connect to the device
    * C4:SendToNetwork(idBinding, nPort, strData)   -- send (append "\n")
    * ReceivedFromNetwork(binding, port, data, from)-- inbound; must buffer
    * OnNetworkBindingChanged(idBinding, bIsBound)  -- address set/removed
    * OnConnectionStatusChanged(idBinding, nPort, strStatus) -- ONLINE/OFFLINE
    * C4:GetBindingAddress(idBinding)               -- the device IP
    * Room: C4:RoomGetId(); C4:GetDeviceVariable(roomId,varId); C4:SendToDevice(...)
    * Don't call network/timer/JSON APIs in OnDriverInit.
=========================================================================== --]]

PROTO_VERSION = 1
DRIVER_VERSION = "dev"   -- placeholder; build.sh stamps the real version (version.txt) into the .c4z
BINDING_NET = 6001
DEVICE_PORT = 6700   -- must match the <connection> port in driver.xml + firmware
-- SDDP self-bind. The keypad is a TCP server the driver dials, but C4:AddDevice
-- leaves binding 6001 with no address and there is no way to set it via AddDevice.
-- So the driver finds ITS OWN keypad on the LAN via SDDP and sets the address
-- itself — the documented C4:SetBindingAddress path, mirroring Snap One's own
-- drivers-common-public/module/ssdp.lua. Robust to DHCP: every announce re-resolves.
SDDP_GROUP = "239.255.255.250"
SDDP_PORT  = 1902
MAC_KEY    = "nv_target_mac"   -- persisted MAC of our keypad, so re-binds survive reloads
IP_KEY     = "nv_target_ip"    -- persisted LAN IP from the agent handoff. C4:SetBindingAddress
                               -- is RUNTIME-only (the project record reads back empty), so
                               -- without this a Director restart drops every keypad to
                               -- "Not bound" until the next hourly agent sync.
HWID_VAR   = "NV_HWID"         -- read-only variable exposing our NuVoxel hardwareId, so the
                               -- agent can reconcile this instance by identity, not name.
HWID_KEY   = "nv_hwid"         -- persisted hardwareId (see gHwidFromDevice below).
                               -- NOT always the MAC: ESP32 boards derive the hwid FROM the
                               -- MAC, but the T3 derives it from the RK3188 eFuse
                               -- (SHA-256(efuse_val)[:12]). The device reports the real one
                               -- in its manifest (mf.hwid); that value wins.
KEYPAD_BINDING = 5002 -- keypad proxy (programmable on-screen buttons + LEDs)
BTN_LINK_BASE  = 300  -- per-button BUTTON_LINK binding id = BTN_LINK_BASE + BUTTON_ID
-- Buttons 1..6 are declared STATICALLY in driver.xml (connections 301-306). That set is
-- frozen: removing a connection corrupts an installed project, so it stays even on a
-- device that reports fewer. Everything above it is created at runtime — see
-- EnsureButtonBindings(). This is NOT a button count; ButtonCount() is.
KEYPAD_STATIC  = 6
-- Hard ceiling on runtime-created bindings, purely a sanity bound: binding ids must
-- stay well clear of the proxies (5002/5003) and the network binding (6001), and a
-- garbage caps.buttons from a hostile/buggy device must not spray hundreds of bindings
-- into the project. 300+64 = 364 leaves an enormous margin.
KEYPAD_MAX     = 64
-- Intercom is a SEPARATE driver now (NuVoxelKeypadIntercom.c4z), because the
-- Communication agent V2 only enrolls a third-party intercom when intercomproxy is
-- the device's PRIMARY proxy — which it can't be here (keypad_proxy is primary). So
-- the intercom lives in its own intercomproxy-primary driver, and this keypad relays
-- the device's SIP/call traffic to/from it over a control binding (the firmware :6700
-- server is single-client, so there's no second connection). See INTERCOM-*.md.
RELAY_BINDING  = 700  -- CONTROL binding the companion intercom driver consumes
INTERCOM_C4Z   = "NuVoxelKeypadIntercom.c4z"  -- the companion intercom endpoint driver

print("NuVoxelKeypad: driver.lua loading v" .. DRIVER_VERSION)

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

-- No intercom here — it's the separate NuVoxelKeypadIntercom.c4z (see RELAY_BINDING
-- above). This driver only RELAYS the device's SIP/call lines to/from it.

local ROOM_VAR = {
  CURRENT_SELECTED_DEVICE = 1000, CURRENT_AUDIO_DEVICE = 1001,
  CURRENT_VIDEO_DEVICE = 1002, CURRENT_MEDIA = 1005, POWER_STATE = 1010,
  CURRENT_VOLUME = 1011, IS_MUTED = 1018, CURRENT_MEDIA_INFO = 1031,
  PLAYING_AUDIO_DEVICE = 1036,
}

-- Runtime state -------------------------------------------------------------
local gPollMs    = 1000
local gConnected = false
local gRoom      = nil            -- the room this instance is placed in
local gRoomSessionDev = nil       -- our room's current media-session device id (the
                                  -- <deviceid> in room var 1031). "" = idle/no session,
                                  -- nil = not learned yet. Used to early-out media-session
                                  -- events that belong to OTHER rooms' sessions.
local gAudioDev = nil             -- last-seen digital-audio session device id. Its var
                                  -- 1006 holds ALL queues house-wide (session->rooms),
                                  -- so remember it for the rooms list even when idle.
local gRxBuf     = ""
local gLastState = nil            -- last pushed state JSON (dedupe)
local gPollTimer = nil
local gSources   = nil            -- cached source list (reset on connect)
local gFavorites = nil            -- cached room favorites keyed by favId -> descriptor, built in
                                  -- BuildFavoritesList, read by DoPlayFavorite (path-2 navigator favorites)
local gButtons   = nil            -- keypad button state {id,label,color,on}, set by the proxy
gLastPlaying     = false          -- last resolved play state (BuildState -> playpause)
local gKpTimer   = nil            -- debounce timer for keypad re-init (L2)
local gThumbUp   = nil            -- {dev,cmd} for the source's thumbs-up PROTOCOL command
local gThumbDown = nil            -- {dev,cmd} for thumbs-down
local gShuffle   = nil            -- {dev,cmd} for ToggleShuffle (source PROTOCOL; pure toggle)
local gRepeat    = nil            -- {dev,cmd} for ToggleRepeat
local gReconnectTimer = nil       -- one-shot OFFLINE re-dial timer
local gPushTimers = {}            -- PushStateSoon one-shots (cancelled on destroy)
local gPushPending = nil          -- coalescing timer: collapse a burst of refresh triggers
                                  -- into one BuildState (see SchedulePush). Cancelled on destroy.
local PUSH_COALESCE_MS = 250      -- max BuildState rate under an event/var storm (streaming)
-- OS3+ MediaSession system events are the fast push path for now-playing/volume/mute;
-- the room-var poll stays as the correctness backstop (these events are undocumented and
-- may not fire on every controller/OS). When events ARE arriving we slow the poll to a
-- heartbeat so it stops generating steady traffic (the Send-Q backlog seen under load),
-- and snap it back to fast the instant events go quiet. See RegisterMediaSessionEvents.
local EVENT_HEARTBEAT_MS = 10000  -- slow poll while events carry state (safety net, not the driver)
local EVENT_QUIET_MS     = 30000  -- no event this long => events unreliable here; resume polling
-- ...but "no events" is the NORMAL condition for a room that is off or idle: nothing
-- is changing, so nothing fires. The old code read that as "events are unreliable"
-- and dropped to the fast poll (1000 ms by default) FOREVER, which is exactly
-- backwards -- an idle room got the heaviest polling, and with several keypads that
-- is a per-second BuildState storm against the Director's single Lua thread. A room
-- that is off only needs a slow correctness backstop; the fast rate is for a room
-- that is ON, where events should be flowing and their absence is suspicious.
local IDLE_POLL_MS  = 15000       -- room OFF: slow backstop, events will wake us
local QUIET_POLL_MS = 5000        -- room on but not playing: nothing is moving
local gLastPower    = false       -- last resolved room power (BuildState)
local gEventDriven   = false      -- true while MediaSession events are actively arriving
local gEventDebounce = nil        -- one-shot: coalesces an event burst into a single PushState
local gEventQuiet    = nil        -- one-shot: resumes fast polling if events stop arriving
local gRegisteredEvents = {}      -- system-event ids we subscribed to (unregistered on destroy)
local gMediaEventNames  = {}      -- set of event NAMES we care about (OnSystemEvent matches by name)
local gDeviceIntercom = false     -- device reported SIP-intercom support in its `hello`
local gMaxButtons = KEYPAD_STATIC -- max keypad buttons the device supports (manifest caps.buttons; an "up to")
local gBtnBindings = {}           -- runtime BUTTON_LINK bindings we own: [bindingId] = name

-- ── Intercom relay (to the companion NuVoxelKeypadIntercom.c4z) ──────────────
-- Intercom is a separate intercomproxy-primary driver (so the Communication agent
-- enrolls it); it reaches the device only through us — we own the :6700 link and
-- relay SIP/call lines both ways over the RELAY_BINDING control binding.
--   us -> intercom : MMK_HELLO {from}  (our Lua device id)  |  MMK_RX {json} (a device line)
--   intercom -> us : MMK_WHOIS {}      (re-announce our id) |  MMK_TX {json} (line to write :6700)
local gRelayRx  = 0     -- MMK_TX msgs received from the intercom driver
local gRelayTx  = 0     -- MMK_RX/HELLO msgs sent to the intercom driver
local gIntercom = nil   -- intercom driver's Lua device id (learned from its msgs' `from`)

local function myDeviceId()
  local ok, id = pcall(function() return C4:GetDeviceID() end)
  return (ok and id) or 0
end
local function relayConsumers()
  local ok, devs = pcall(function() return C4:GetBoundConsumerDevices(0, RELAY_BINDING) end)
  return (ok and type(devs) == "table") and devs or {}
end
local function relayCount() local n = 0; for _ in pairs(relayConsumers()) do n = n + 1 end; return n end
-- Prefer the id learned from the intercom driver's `from` (its real Lua device, which
-- reaches ExecuteCommand); else the bound-consumer list.
local function relayTargets()
  if gIntercom then return { gIntercom } end
  local t = {}; for id in pairs(relayConsumers()) do t[#t + 1] = id end; return t
end
function RelayStatus()
  pcall(function()
    C4:UpdateProperty("Intercom Relay", string.format(
      "consumers=%d  ic=%s  rx=%d  tx=%d  dev=%s", relayCount(),
      tostring(gIntercom or "?"), gRelayRx, gRelayTx, gConnected and "connected" or "offline"))
  end)
end
function RelayHello()   -- tell the bound intercom driver our device id
  local me = myDeviceId()
  for _, id in ipairs(relayTargets()) do
    pcall(function() C4:SendToDevice(id, "MMK_HELLO", { from = me }) end); gRelayTx = gRelayTx + 1
  end
  RelayStatus()
end
function RelayToIntercom(line)   -- forward one device sipstate/callstate line
  for _, id in ipairs(relayTargets()) do
    pcall(function() C4:SendToDevice(id, "MMK_RX", { json = line }) end); gRelayTx = gRelayTx + 1
  end
  RelayStatus()
end
-- Set from the MMK_TX / MMK_WHOIS handlers in ExecuteCommand (below).
function RelaySetIntercom(from) if from then gIntercom = tonumber(from) or gIntercom end end
function RelayRxTick(json)
  gRelayRx = gRelayRx + 1
  if json and gConnected then pcall(function() C4:SendToNetwork(BINDING_NET, DEVICE_PORT, tostring(json) .. "\n") end) end
  RelayStatus()
end
local gDeviceManifest = nil       -- device's self-described manifest (model/hw ids/connectivity/power/caps)
local gAggWatchId = nil          -- digital-audio aggregator device currently var-watched
local gDevHwid   = nil            -- device identity (from hello), pinned on first use
local gDevSku    = nil

-- normalize a MAC to lowercase hex, no separators. Defined up here (not beside
-- the SDDP code that also uses it) because OnDriverLateInit calls it — a `local
-- function` further down would still be a nil global at that call site.
local function normMac(s) return (tostring(s or ""):lower():gsub("[^0-9a-f]", "")) end

-- ============================================================================
-- Logging
-- ============================================================================
-- Every argument goes through tostring(): table.concat() ERRORS on a boolean or a
-- table, and on a nil in the middle it either errors or silently truncates the line
-- (a hole makes #t undefined). Dozens of call sites pass values that can be nil --
-- e.g. `resp and resp.code` -- and a throw here would abort whatever handler was
-- logging, including callbacks that are not pcall-wrapped. Logging must never be
-- able to break the thing it is describing.
--
-- The whole line is also capped: some call sites log device-supplied text (a bad
-- frame can be 16 KB), and with Debug Logging = Log every line lands in the
-- Director's error log on a controller with very little free space.
local DBG_MAX = 400
local function dbg(...)
  local mode = Properties and Properties["Debug Logging"] or "Print"
  if mode == "Off" then return end
  local n, parts = select("#", ...), {}
  for i = 1, n do parts[i] = tostring((select(i, ...))) end
  local msg = "NuVoxelKeypad: " .. table.concat(parts, " ")
  if #msg > DBG_MAX then msg = msg:sub(1, DBG_MAX) .. "…(+" .. (#msg - DBG_MAX) .. ")" end
  if mode == "Print" or mode == "Print and Log" then print(msg) end
  if (mode == "Log" or mode == "Print and Log") and C4.ErrorLog then C4:ErrorLog(msg) end
end

-- Proxy-command tracing was a dev-only capture path: a "Diagnostics Sink" property
-- holding an http:// URL, and every proxy command in AND out was GET-ed to it.
-- It answered a real question (Director's driver_event.log records what it
-- DISPATCHES to a proxy, not what actually reaches this Lua), but it is an
-- arbitrary-URL exfiltration channel for live system traffic sitting behind one
-- editable string property — not something to ship. Removed from release builds.
-- To capture a session in dev, restore this from git and rebuild locally.

-- The TCP link's state. Writes "Link Status", NOT "Connection": there used to be two
-- different properties both called "Connection" (this one, and the device's
-- ethernet/wifi link type under Firmware), so this setter and the manifest handler
-- overwrote each other. They are now "Link Status" and "Network Link".
local function setStatus(s) C4:UpdateProperty("Link Status", s) end

-- SetTimer returns a timer object (:Cancel) on current cores but a numeric handle on
-- others — cancel either form so nothing keeps firing (esp. across driver destroy).
local function cancelTimer(t)
  if not t then return end
  if type(t) == "table" and t.Cancel then pcall(function() t:Cancel() end)
  else pcall(function() C4:KillTimer(t) end) end
end

-- ============================================================================
-- Lifecycle
-- ============================================================================
function OnDriverInit(initType)
  print("NuVoxelKeypad: OnDriverInit (" .. tostring(initType) .. ")")
  C4:UpdateProperty("Driver Version", DRIVER_VERSION)
  setStatus("Not bound")
  -- Publish our identity as a read-only, hidden variable (see HWID_VAR). Created
  -- empty here; filled once we learn our MAC (persist load / hello).
  pcall(function() C4:AddVariable(HWID_VAR, "", "STRING", true, true) end)
  -- FIRST, before anything can touch a binding id: dynamic bindings do not survive a
  -- Director restart, so re-create the ones we own or every link past button 6 in the
  -- project is lost. Neither a network, timer nor JSON call, so it is legal here.
  RestoreButtonBindings()
end

function OnDriverLateInit(initType)
  print("NuVoxelKeypad: OnDriverLateInit")
  gPollMs = tonumber(Properties["Room Poll Interval (ms)"]) or 1000
  local ok, r = pcall(function() return C4:RoomGetId() end)
  if ok and r and r ~= 0 then gRoom = tostring(r) end
  dbg("room =", tostring(gRoom))
  -- State now, proxy registration on the debounce: the BUTTON_LINK bindings land
  -- moments from here and each used to force its own re-register. EnsureButtons()
  -- makes gButtons usable immediately regardless.
  EnsureButtons()
  ScheduleInitKeypad()
  WatchRoomVars()
  StartPolling()
  RegisterMediaSessionEvents()   -- OS3+ push path; degrades silently to poll-only if absent
  -- SDDP self-bind. Prefer the manual override, else the MAC we persisted from a
  -- prior hello / agent handoff. With a target and no address yet, go find it.
  local ovr = Properties and Properties["Device MAC Override"]
  if ovr and normMac(ovr) ~= "" then gTargetMac = normMac(ovr)
  else pcall(function() gTargetMac = normMac(C4:PersistGetValue(MAC_KEY)) end) end
  if gTargetMac == "" then gTargetMac = nil end
  -- Re-publish identity on every load so an instance added by an older build (or
  -- whose variable was cleared on reload) becomes reconcilable without waiting for
  -- a fresh handoff. Prefer the persisted hardwareId (authoritative, device-reported
  -- on some earlier hello) and fall back to the MAC only if we've never had one.
  local savedHwid = ""
  pcall(function() savedHwid = normMac(C4:PersistGetValue(HWID_KEY)) end)
  if savedHwid ~= "" then
    gHwidFromDevice = true
    pcall(function() C4:SetVariable(HWID_VAR, savedHwid) end)
  elseif gTargetMac then
    pcall(function() C4:SetVariable(HWID_VAR, gTargetMac) end)
  end
  -- Restore the agent's last known LAN IP onto the binding. The binding address does
  -- not survive a restart, so without this we'd sit "Not bound" until the next agent
  -- sync (up to an hour). Only when the binding is genuinely empty — a dealer's manual
  -- address on the Connections page must win.
  local haveAddr = ""
  pcall(function() haveAddr = tostring(C4:GetBindingAddress(BINDING_NET) or "") end)
  if haveAddr == "" then
    local savedIp = ""
    pcall(function() savedIp = tostring(C4:PersistGetValue(IP_KEY) or "") end)
    savedIp = savedIp:match("^%s*(%d+%.%d+%.%d+%.%d+)%s*$")
    if savedIp then
      dbg("restoring persisted target IP", savedIp)
      pcall(function() C4:SetBindingAddress(BINDING_NET, savedIp) end)
    end
  end
  -- If the binding already has an address (re-init), connect now; else discover.
  TryConnect()
  StartSddp()

end

-- Called before the driver is reloaded (DIT_UPDATING) or removed/shutdown (DIT_LOADED).
-- Tear down EVERYTHING that could fire into a half-reloaded driver: all timers + the
-- outbound network connection. Must be safe to call in any state.
function OnDriverDestroyed(initType)
  dbg("OnDriverDestroyed (" .. tostring(initType) .. ")")
  gConnected = false
  StopPolling()
  UnwatchRoomVars()
  UnwatchAggregator()
  UnregisterMediaSessionEvents()          -- drop the system-event subscriptions
  cancelTimer(gEventDebounce);  gEventDebounce = nil
  cancelTimer(gEventQuiet);     gEventQuiet    = nil
  gEventDriven = false
  cancelTimer(gKpTimer);        gKpTimer = nil
  cancelTimer(gReconnectTimer); gReconnectTimer = nil
  cancelTimer(gPushPending);    gPushPending = nil
  for _, t in ipairs(gPushTimers) do cancelTimer(t) end
  gPushTimers = {}
  StopSddp()     -- release the scratch SDDP binding + its re-search timer
  pcall(function() C4:NetDisconnect(BINDING_NET, DEVICE_PORT) end)
end

function OnPropertyChanged(prop)
  dbg("Property changed:", prop, "=", tostring(Properties[prop]))
  if prop == "Device MAC Override" then
    local m = normMac(Properties[prop])
    if m ~= "" then SetTargetMac(m, "manual override") end
  end
  if prop == "Room Poll Interval (ms)" then
    gPollMs = tonumber(Properties[prop]) or 1000
    StartPolling()
  elseif prop == "Display Orientation" or prop == "Layout"
      or prop == "Background" or prop == "Show Title" or prop == "Show Artist/Album"
      or prop == "Show Info Button" or prop == "Show Progress Bar"
      or prop == "Active Brightness" or prop == "Idle Timeout" or prop == "Idle Brightness" then
    if gConnected then pcall(PushState, true) end   -- device persists + applies live
  elseif prop == "Halo Color" or prop == "Halo Ring Color" or prop == "Halo Brightness" then
    if gConnected then pcall(PushHalo) end
  end
end

function ExecuteCommand(cmd, params)
  dbg("ExecuteCommand:", cmd)
  -- Composer Actions arrive as LUA_ACTION with the <command> in params.ACTION — normalize
  -- to the command name so the dispatch below matches (SDK-standard idiom; without this the
  -- entire Actions tab silently no-ops).
  if cmd == "LUA_ACTION" and params and params.ACTION then
    cmd = tostring(params.ACTION); params.ACTION = nil
    dbg("  action ->", cmd)
  end
  -- A caller can hand us our keypad's MAC (e.g. right after AddDevice) so we can
  -- SDDP-find and bind it. (Also received in ReceivedFromProxy as a belt —
  -- SendToDevice delivery to a proxy device isn't documented precisely.)
  if cmd == "NV_SET_TARGET_MAC" and params and params.mac then
    SetTargetMac(params.mac, "agent handoff"); return
  end
  -- A caller can hand us our device's current LAN IP so we bind directly —
  -- bypassing unreliable multicast SDDP.
  if cmd == "NV_SET_TARGET_IP" and params and params.ip then
    SetTargetIp(params.ip, "agent handoff"); return
  end
  -- Intercom relay (driver<->driver): the companion NuVoxelKeypadIntercom driver
  -- writes SIP/call JSON onto our :6700 link through us, and we relay device lines back.
  if cmd == "MMK_TX" then
    RelaySetIntercom(params and params.from); RelayRxTick(params and params.json); return
  elseif cmd == "MMK_WHOIS" then
    RelaySetIntercom(params and params.from); RelayHello(); return
  elseif cmd == "BindIntercom" then
    -- Bind the relay to the companion intercom driver by its device id (C4:Bind
    -- must be called by one of the two parties). Idempotent.
    local id = params and tonumber(params.deviceId)
    if id then
      pcall(function() C4:Bind(myDeviceId(), RELAY_BINDING, id, RELAY_BINDING, "MMKEYPAD_INTERCOM") end)
      dbg("BindIntercom: bound relay to intercom device", id); RelayHello()
    end
    return
  end
  if cmd == "IdentifyDevice" or cmd == "Identify" or cmd == "IDENTIFY" then
    SendIdentify()
  elseif cmd == "Reconnect" then
    TryConnect()
    StartSddp()   -- if unbound, (re)discover our keypad on the LAN and bind it
  elseif cmd == "ForgetDevice" then
    ForgetDevice()
  elseif cmd == "ReRegister" then
    -- SIP now lives in the companion intercom driver; re-hello so it re-provisions.
    RelayHello()
  elseif cmd == "PlayAnnouncement" then
    local text  = params and tostring(params.Text or "")
    local chime = not (params and tostring(params.Chime or "Yes") == "No")
    SendAnnounce(text, chime)
  elseif cmd == "SetButtonLED" then
    -- Drive a button's LED from Programming (the point of the per-button
    -- "Manual (Programming)" LED mode). Works in either mode; Manual just means
    -- MATCH_LED_STATE tracking won't fight it.
    local n = tonumber(params and params.Button)
    local b = n and gButtons and gButtons[n]
    if b then
      local s = tostring(params and params.State or "On"):lower()
      b.on = (s == "on" or s == "true" or s == "1")
      PushState(true)
      dbg("SetButtonLED", n, "->", tostring(b.on))
    end
  elseif cmd == "SetButtonLEDColor" then
    local n = tonumber(params and params.Button)
    local b = n and gButtons and gButtons[n]
    local hex = NamedHex(params and params.Color)
    -- Programming owns this at runtime; it is not a dealer edit. It does change a
    -- fingerprinted field though, so drop the cached print or a later edit back to the
    -- old value would look like a no-op.
    if b and hex then b.color = hex; gKpPrintInvalidate(); PushState(true); dbg("SetButtonLEDColor", n, hex) end
  elseif cmd == "SetButtonLabel" then
    local n = tonumber(params and params.Button)
    local b = n and gButtons and gButtons[n]
    if b then
      b.label = tostring(params and params.Text or "")
      gKpPrintInvalidate(); PushState(true); dbg("SetButtonLabel", n, b.label)
    end
  elseif cmd == "SetHalo" then
    SendHalo(params and params.Color, params and params.Brightness)
  elseif cmd == "RebootDevice" then
    Send({ t = "reboot" })
    dbg("sent reboot")
  end
end

-- ============================================================================
-- SDDP self-bind: find OUR keypad on the LAN and point binding 6001 at it.
-- ============================================================================
gSddpBind  = nil    -- scratch UDP binding grabbed from 6900-6999 to hear SDDP
gTargetMac = nil    -- hex, lowercase, no separators — which physical keypad is ours
-- True once the DEVICE told us its own hardwareId (manifest mf.hwid). Until then we
-- fall back to publishing the MAC, which is correct only on boards whose hwid IS the
-- MAC. Once the device has spoken it is authoritative and the MAC must not clobber it.
gHwidFromDevice = false
gSddpTimer = nil    -- re-SEARCH cadence until bound

-- A free scratch binding = one whose address reads empty (Snap One's ssdp.lua trick).
local function grabScratchBinding()
  for i = 6999, 6900, -1 do
    local ok, addr = pcall(function() return C4:GetBindingAddress(i) end)
    if ok and (addr == nil or tostring(addr) == "") then return i end
  end
  return nil
end

-- The keypad replies to any datagram starting "SEARCH" (firmware sddp.c), sending
-- its announce to us — faster than waiting for the periodic NOTIFY ALIVE.
function SddpSearch()
  if gSddpBind then pcall(function() C4:SendToNetwork(gSddpBind, SDDP_PORT, "SEARCH * SDDP/1.0\r\n\r\n") end) end
end

function StartSddp()
  if not gTargetMac or gTargetMac == "" then return end   -- nothing to look for yet
  if C4:GetBindingAddress(BINDING_NET) ~= "" then return end -- already bound; nothing to do
  if gSddpBind then SddpSearch(); return end               -- already listening
  gSddpBind = grabScratchBinding()
  if not gSddpBind then dbg("sddp: no free scratch binding 6900-6999"); return end
  pcall(function() C4:CreateNetworkConnection(gSddpBind, SDDP_GROUP) end)
  pcall(function() C4:NetConnect(gSddpBind, SDDP_PORT, "UDP") end)
  dbg("sddp: listening (scratch " .. gSddpBind .. ") for our keypad mac " .. gTargetMac)
  SddpSearch()
  -- Re-search every 5s until we're bound; a keypad still booting will answer a later one.
  cancelTimer(gSddpTimer)
  gSddpTimer = C4:SetTimer(5000, function() SddpSearch() end, true)
end

function StopSddp()
  cancelTimer(gSddpTimer); gSddpTimer = nil
  if gSddpBind then
    pcall(function() C4:NetDisconnect(gSddpBind, SDDP_PORT) end)
    pcall(function() C4:SetBindingAddress(gSddpBind, "") end)  -- release the scratch binding
    gSddpBind = nil
  end
end

-- Parse one SDDP datagram; if it's OUR keypad, point 6001 at its announced IP.
function HandleSddp(data, from)
  if type(data) ~= "string" or not data:find("NuVoxel:keypad") then return end
  local host = data:match('Host:%s*"?([^"\r\n]+)')
  -- Prefer the TRANSPORT source address over the From: line in the payload. The
  -- payload is attacker-controlled text: taking the IP from it let any host on the
  -- LAN redirect this driver's device connection to an address it does not even
  -- own, and then speak the keypad protocol to us (fire button programming, hand
  -- us an identity). The sender can still lie about WHO it is, but that is all.
  local ip = tostring(from or ""):match("^(%d+%.%d+%.%d+%.%d+)")
  if not ip then ip = data:match('From:%s*"?([%d%.]+)') end
  if not host or not ip then return end
  -- Is this OUR unit? Match the announced host against the MAC *or* the hardware
  -- id. A T3 announces itself by hwid ("T3-10-75d45f4b3dba0e83c4d4dad0"), not by
  -- MAC, so a MAC-only test could never match one -- meaning a T3 that changed
  -- DHCP address was never re-bound and simply stayed "not connected" until
  -- someone re-entered its address by hand. Two panels in the field were sitting
  -- in exactly that state, replying to every SEARCH while the driver ignored them.
  -- The announcement may also carry an explicit MAC header. Host is
  -- "<variant>-<hardware_id>" and on a T3 the hardware id is NOT derived from the
  -- MAC, so matching the host alone can never identify a T3 to a driver that knows
  -- it by MAC. Prefer the explicit header when present.
  local advMac = normMac(data:match('MAC:%s*"?([0-9A-Fa-f:%-]+)') or "")
  local nh = normMac(host)
  local known = ""
  pcall(function() known = normMac(C4:PersistGetValue(HWID_KEY)) end)
  if known == "" and gDevHwid then known = normMac(gDevHwid) end
  local mine = (gTargetMac and gTargetMac ~= "" and
                 (advMac == gTargetMac or nh:find(gTargetMac, 1, true)))
            or (known ~= "" and (advMac == known or nh:find(known, 1, true)))
  if not mine then return end
  local cur = ""
  pcall(function() cur = C4:GetBindingAddress(BINDING_NET) or "" end)
  if cur ~= ip then
    dbg("sddp: our keypad at " .. ip .. " (was " .. (cur == "" and "unbound" or cur) .. ") — binding")
    -- SetBindingAddress + NetConnect are the documented pair (both banned in OnDriverInit;
    -- we only ever reach here from LateInit/timer/received-data, which is fine).
    pcall(function() C4:SetBindingAddress(BINDING_NET, ip) end)
    pcall(function() C4:NetConnect(BINDING_NET, DEVICE_PORT, "TCP") end)
  end
  -- Bound now — stop the re-search cadence but keep listening so a DHCP move re-binds.
  cancelTimer(gSddpTimer); gSddpTimer = nil
end

-- Set (and persist) which keypad is ours, then (re)start discovery. Called from the
-- device's own hello (authoritative), the agent handoff, or the manual override.
-- Publish this instance's NuVoxel hardwareId for the agent to reconcile against, and
-- persist it so a reload/restart doesn't drop us back to name-match. `fromDevice`
-- marks the authoritative value (the device's own manifest) — once set, the MAC
-- fallback stops writing.
function PublishHwid(hwid, why, fromDevice)
  local h = normMac(hwid)   -- same normalisation the agent's normHwid() applies
  if h == "" then return end
  if gHwidFromDevice and not fromDevice then return end
  if fromDevice then gHwidFromDevice = true end
  pcall(function() C4:PersistSetValue(HWID_KEY, h) end)
  pcall(function() C4:SetVariable(HWID_VAR, h) end)
  dbg("hwid =", h, "(" .. tostring(why) .. ")")
end

-- Unpair this driver instance from the keypad it is bound to.
--
-- The identity pin (see the hello handler) deliberately refuses a hwid that differs
-- from the paired one, so swapping in NEW hardware -- an RMA, a bench spare, moving
-- a driver to a different panel -- needs an explicit way to say "this is a different
-- keypad now". That is a dealer action rather than something a packet can trigger:
-- being able to re-pair from the network is exactly the hole the pin closes.
--
-- Clears every piece of pairing state: the hwid, the MAC discovery target, the
-- remembered IP, the live identity used for licence relay, and the binding address.
-- Then it goes back to discovery, so the next keypad that announces itself can pair.
function ForgetDevice()
  dbg("Forget Device: clearing pairing (hwid, mac, ip, binding)")
  StopSddp()
  pcall(function() C4:NetDisconnect(BINDING_NET, DEVICE_PORT) end)
  gConnected = false
  gTargetMac, gHwidFromDevice = nil, false
  gDevHwid, gDevSku = nil, nil
  for _, k in ipairs({ HWID_KEY, MAC_KEY, IP_KEY }) do
    pcall(function() C4:PersistSetValue(k, "") end)
  end
  pcall(function() C4:SetVariable(HWID_VAR, "") end)
  pcall(function() C4:SetBindingAddress(BINDING_NET, "") end)
  pcall(function() C4:UpdateProperty("MAC Address", "") end)
  setStatus("Unpaired — waiting for a keypad")
  StartSddp()   -- open to whichever keypad announces itself next
end

function SetTargetMac(mac, why)
  local m = normMac(mac)
  if m == "" or m == gTargetMac then return end
  gTargetMac = m
  pcall(function() C4:PersistSetValue(MAC_KEY, m) end)
  -- Only a provisional identity: right on ESP32 boards, wrong on the T3 (eFuse hwid).
  -- The device's manifest overrides it and, once it has, must not be clobbered here.
  if not gHwidFromDevice then PublishHwid(m, "mac fallback") end
  dbg("target keypad mac =", m, "(" .. tostring(why) .. ")")
  StartSddp()
end

-- MAC->IP handoff. A caller (e.g. right after AddDevice) can hand us the device's
-- current LAN IP; we point binding 6001 at it and dial directly — the reliable path
-- when multicast SDDP discovery is flaky (WiFi APs / cross-subnet). Idempotent:
-- no-op if we're already connected to this IP.
function SetTargetIp(ip, why)
  ip = tostring(ip or ""):match("^%s*(%d+%.%d+%.%d+%.%d+)%s*$")
  if not ip then return end
  -- Persist FIRST, before any early-out. SetBindingAddress is runtime-only, so this
  -- is the only copy that survives a Director restart (see IP_KEY) — and the common
  -- steady state is "already connected to this IP", which returns below. Persisting
  -- after that guard meant an already-healthy fleet never wrote the key at all, and
  -- every keypad still came back "Not bound" after a restart (observed).
  pcall(function() C4:PersistSetValue(IP_KEY, ip) end)
  local cur = ""
  local ok, addr = pcall(function() return C4:GetBindingAddress(BINDING_NET) end)
  if ok and addr then cur = tostring(addr) end
  if ip == cur and gConnected then return end   -- already bound + connected here
  dbg("target IP =", ip, "(" .. tostring(why) .. ")")
  pcall(function() C4:SetBindingAddress(BINDING_NET, ip) end)
  pcall(function() C4:NetConnect(BINDING_NET, DEVICE_PORT, "TCP") end)
end

-- ============================================================================
-- Network binding (driver connects OUT to the device)
-- ============================================================================
function TryConnect()
  local ok, addr = pcall(function() return C4:GetBindingAddress(BINDING_NET) end)
  if ok and addr and tostring(addr) ~= "" then
    dbg("connecting to", tostring(addr), ":", DEVICE_PORT)
    setStatus("Connecting " .. tostring(addr))
    pcall(function() C4:NetConnect(BINDING_NET, DEVICE_PORT, "TCP") end)
  else
    dbg("no binding address yet (bind the device on the Connections page)")
    setStatus("Not bound")
  end
end

function OnNetworkBindingChanged(idBinding, bIsBound)
  dbg("OnNetworkBindingChanged", idBinding, tostring(bIsBound))
  if idBinding ~= BINDING_NET then return end
  if bIsBound then TryConnect()
  else gConnected = false; setStatus("Not bound") end
end

-- Control bindings. The RELAY_BINDING (700) carries SIP/call JSON to the companion
-- NuVoxelKeypadIntercom.c4z; handled below.
function OnBindingChanged(idBinding, strClass, bIsBound, otherDeviceID, otherBindingID)
  dbg("OnBindingChanged", idBinding, tostring(strClass), tostring(bIsBound))
  -- A per-button BUTTON_LINK (static 301-306 or one we created) was (un)bound → re-label
  -- from the linked device (bind: auto-name; unbind: revert to the manual name /
  -- "Button N").
  if IsButtonLinkBinding(idBinding) then
    ScheduleInitKeypad()
  elseif idBinding == RELAY_BINDING and bIsBound then
    -- The companion intercom driver bound our relay — announce our device id so it
    -- can start relaying SIP/call traffic (it replies with MMK_WHOIS / provisions SIP).
    gIntercom = tonumber(otherDeviceID) or gIntercom
    RelayHello()
  elseif idBinding == RELAY_BINDING then
    gIntercom = nil; RelayStatus()
  end
end

function OnConnectionStatusChanged(idBinding, nPort, strStatus)
  if idBinding ~= BINDING_NET then return end
  dbg("OnConnectionStatusChanged", nPort, strStatus)
  if strStatus == "ONLINE" then
    local was = gConnected
    gConnected = true
    gRxBuf = ""; gLastState = nil; gSources = nil
    local ok, addr = pcall(function() return C4:GetBindingAddress(BINDING_NET) end)
    setStatus("Connected " .. (ok and tostring(addr) or ""))
    PushState(true)
    PushHalo()                   -- push the configured halo LED color/brightness
    if not was then
      pcall(function() C4:FireEvent("Device Connected") end)
      -- Auto firmware check on the first connect this session: tell the device to run
    end
  else
    local was = gConnected
    gConnected = false
    setStatus("Offline")
    RelayStatus()                -- refresh the relay diagnostic (the intercom detects the drop via SIP)
    if was then pcall(function() C4:FireEvent("Device Disconnected") end) end
    -- H1: auto re-dial with a short backoff. The Director usually re-dials a bound
    -- IP device, but don't rely on it — a power-cycled device must reconnect on its
    -- own rather than sit Offline until a manual Reconnect.
    cancelTimer(gReconnectTimer)
    gReconnectTimer = C4:SetTimer(5000, function()
      gReconnectTimer = nil
      if not gConnected then TryConnect() end
    end, false)
  end
end

-- Inbound from the device — buffer and split on "\n" (may arrive in fragments).
function ReceivedFromNetwork(idBinding, nPort, strData, strFrom)
  if idBinding == gSddpBind then HandleSddp(tostring(strData), strFrom); return end
  if idBinding ~= BINDING_NET then return end
  gRxBuf = gRxBuf .. tostring(strData)
  while true do
    local nl = gRxBuf:find("\n")
    if not nl then break end
    local line = gRxBuf:sub(1, nl - 1):gsub("%s+$", "")
    gRxBuf = gRxBuf:sub(nl + 1)
    if #line > 0 then HandleMessage(line) end
  end
  -- M5: only drop a genuinely stuck oversized line (a huge buffer with no newline),
  -- never wipe mid-stream while complete lines are still being framed.
  if #gRxBuf > 16384 and not gRxBuf:find("\n") then
    dbg("rx overflow: dropping stuck partial line")
    gRxBuf = ""
  end
end

-- ============================================================================
-- Protocol
-- ============================================================================
-- Minimum gap between device-requested project walks, per query kind. Generous
-- enough that a user tapping "add rooms" never notices, tight enough that a loop
-- on the device cannot become a load problem on the controller.
local QUERY_MIN_GAP_S = 2
local gLastQueryAt, gLastQueryRes = {}, {}
-- Rebuild at most once per gap; inside the gap serve the previous answer instead of
-- dropping the request. A throttle that stays silent would leave the panel waiting
-- on a reply that never comes -- an empty rooms list is a worse bug than a slightly
-- stale one.
function QueryResult(kind, build)
  local now = os.time()
  if now - (gLastQueryAt[kind] or 0) >= QUERY_MIN_GAP_S or gLastQueryRes[kind] == nil then
    gLastQueryAt[kind]  = now
    gLastQueryRes[kind] = build()
  else
    dbg("query", kind, "throttled; serving the previous result")
  end
  return gLastQueryRes[kind]
end

function Send(obj)
  if not gConnected then return end
  local ok, encoded = pcall(json.encode, obj)
  if not ok then dbg("encode failed:", tostring(encoded)); return end
  C4:SendToNetwork(BINDING_NET, DEVICE_PORT, encoded .. "\n")
end

function SendIdentify()
  Send({ t = "identify" })
  dbg("sent identify (connected=" .. tostring(gConnected) .. ")")
end

-- "Halo" RGB LED config. Named colors -> RGB; pushed to the device as a `halo`
-- message (idle color + ring/pulse color + brightness). See firmware halo.c.
local HALO_COLORS = {
  ["Off"]        = {0, 0, 0},     ["White"]   = {255, 255, 255},
  ["Warm White"] = {255, 170, 80},["Blue"]    = {0, 0, 255},
  ["Cyan"]       = {0, 200, 255}, ["Teal"]    = {0, 180, 150},
  ["Green"]      = {0, 255, 40},  ["Amber"]   = {255, 130, 0},
  ["Red"]        = {255, 0, 0},   ["Purple"]  = {150, 0, 255},
  ["Pink"]       = {255, 40, 120},["Magenta"] = {255, 0, 180},
}
function PushHalo()
  local function rgb(name) return HALO_COLORS[name or "Off"] or HALO_COLORS["Off"] end
  local idle   = rgb(Properties and Properties["Halo Color"])
  local ring   = rgb(Properties and Properties["Halo Ring Color"])
  local bright = tonumber(Properties and Properties["Halo Brightness"]) or 50
  Send({ t = "halo", r = idle[1], g = idle[2], b = idle[3],
         pr = ring[1], pg = ring[2], pb = ring[3], bright = bright })
  dbg("pushed halo: idle=" .. tostring(Properties and Properties["Halo Color"]) ..
      " ring=" .. tostring(Properties and Properties["Halo Ring Color"]) .. " bright=" .. bright)
end

-- Named color -> "RRGGBB" hex, for the SetButtonLEDColor programming command (reuses
-- the halo palette). Global fn so ExecuteCommand — defined earlier in the file — can
-- reach the file-local HALO_COLORS at call time. Returns nil for an unknown name.
function NamedHex(name)
  local c = HALO_COLORS[tostring(name or "")]
  return c and string.format("%02x%02x%02x", c[1], c[2], c[3]) or nil
end

-- Set Halo from Programming (SetHalo command). Overrides the idle color/brightness for
-- this push; ring color stays the configured property. Blank args fall back to the props.
function SendHalo(colorName, brightName)
  local idle   = HALO_COLORS[tostring(colorName or "")] or HALO_COLORS[Properties and Properties["Halo Color"]] or {0, 0, 0}
  local ring   = HALO_COLORS[Properties and Properties["Halo Ring Color"]] or idle
  local bright = tonumber(brightName) or tonumber(Properties and Properties["Halo Brightness"]) or 50
  Send({ t = "halo", r = idle[1], g = idle[2], b = idle[3],
         pr = ring[1], pg = ring[2], pb = ring[3], bright = bright })
  dbg("SetHalo: color=" .. tostring(colorName) .. " bright=" .. tostring(bright))
end

-- Play an announcement on the device: a chime out its speaker + the text shown
-- on screen. Triggered from Composer Programming (Device Specific Command
-- "PlayAnnouncement"). text "" = chime only; chime=false = banner only.
function SendAnnounce(text, chime)
  Send({ t = "announce", text = text or "", chime = chime ~= false })
  dbg("sent announce (chime=" .. tostring(chime ~= false) .. "): " .. tostring(text))
  pcall(function() C4:FireEvent("Announcement Played") end)
end






-- This driver's own Control4 device id (0 if unavailable).
local function myDeviceId()
  local ok, id = pcall(function() return C4:GetDeviceID() end); return (ok and id) or 0
end

function HandleMessage(line)
  local ok, msg = pcall(json.decode, line)
  if not ok or type(msg) ~= "table" or not msg.t then dbg("bad msg:", line); return end
  local t = msg.t
  if t == "hello" then
    dbg("hello from device mac=", tostring(msg.mac), "fw=", tostring(msg.fw), "intercom=", tostring(msg.intercom))
    -- The device just told us its MAC over a working connection — the most
    -- authoritative source. Persist it so every future reload/DHCP move can
    -- self-rebind via SDDP with no agent handoff and no dealer.
    if msg.mac then SetTargetMac(msg.mac, "hello") end
    if msg.fw and tostring(msg.fw) ~= "" then
      pcall(function() C4:UpdateProperty("Firmware Version", tostring(msg.fw)) end)
    end
    -- Device advertises whether it has the SIP intercom; gates the visibility of the
    -- intercom properties (the endpoint itself is always present as sub-proxy 5003).
    gDeviceIntercom = (msg.intercom == true)
    -- Device manifest: self-described model / hardware ids / connectivity / power /
    -- capabilities. Store it (for capability-driven behavior) and surface the key
    -- fields as read-only properties so the dealer sees what this keypad is.
    local mf = msg.manifest
    if type(mf) == "table" then
      gDeviceManifest = mf
      if mf.caps and mf.caps.intercom ~= nil then gDeviceIntercom = (mf.caps.intercom == true) end
      -- caps.buttons is now the ONLY thing that decides how many buttons exist, so a
      -- change here has to re-register the proxy (it never did — the count came from a
      -- property that hello could not touch).
      if mf.caps and type(mf.caps.buttons) == "number" and mf.caps.buttons ~= gMaxButtons then
        gMaxButtons = mf.caps.buttons
        -- Past button 6 the BUTTON_LINK bindings don't exist yet — this manifest is the
        -- first time we learn how many this hardware wants. Create them BEFORE the
        -- re-register so Composer's panel and the Connections page agree in one pass.
        EnsureButtonBindings()
        ScheduleInitKeypad()
      end
      local function setp(name, val)
        if val ~= nil and tostring(val) ~= "" then
          pcall(function() C4:UpdateProperty(name, tostring(val)) end)
        end
      end
      setp("Device Model", mf.model)
      setp("MAC Address", mf.mac)
      -- The device is authoritative about its own identity. On the T3 this is the
      -- eFuse hwid, which is NOT mf.mac — publishing it is what lets the agent match
      -- this instance by hwid instead of falling back to room name.
      if mf.hwid then PublishHwid(mf.hwid, "device manifest", true) end
      setp("Network Link", mf.net and mf.net.link)   -- was "Connection"; see setStatus()
      setp("Power Source", mf.power and mf.power.source)
      if mf.fw then setp("Firmware Version", mf.fw) end
      -- Show only the settings this hardware actually has (caps-driven).
      ApplyManifestPropVisibility(mf)
    end
    -- The keypad advertises its identity (hwid/secret/sku) on hello; we pin it on
    -- first use so a spoofed announce can't swap the device we're bound to.
    if msg.hwid and msg.sku then
      -- Pin the identity on FIRST use. SDDP cannot authenticate the peer, so a
      -- spoofed announce can get us to dial an impostor; what it must not also do
      -- is swap the identity we relay licences for. Once a hwid is known (persisted
      -- across reloads by PublishHwid), a DIFFERENT one arriving on this binding is
      -- refused rather than trusted -- re-pairing is a deliberate act (Forget
      -- Device / clearing the persisted hwid), not something a packet can do.
      local claimed = normMac(msg.hwid)
      local known   = ""
      -- Normalise BOTH sides: PublishHwid persists a normalised value today, but a
      -- comparison that depends on that staying true would fail open.
      pcall(function() known = normMac(C4:PersistGetValue(HWID_KEY)) end)
      if known ~= "" and claimed ~= "" and claimed ~= known then
        -- Say so where a dealer will actually see it, and name the way out: a
        -- silent refusal here looks exactly like a dead keypad.
        dbg("REFUSING hello: hwid", claimed, "does not match the paired", known,
            "— run the Forget Device action to pair different hardware")
        setStatus("Wrong keypad (use Forget Device to re-pair)")
        return
      end
      gDevHwid, gDevSku = tostring(msg.hwid), tostring(msg.sku)
    end
    -- `hello` is the real "device is ready" edge: it arrives after the device's app
    -- is up, whereas the TCP ONLINE edge only means the socket connected. So push the
    -- FULL driver-owned config here, not just state. PushHalo used to be missing from
    -- this path, so after a device reboot the halo LED kept whatever it had in NVS and
    -- silently drifted from these properties until someone edited one of them.
    PushState(true)
    PushHalo()
    RelayHello()           -- tell the bound intercom driver the device is up (it re-provisions SIP)
  elseif t == "cmd" then DoTransport(msg.cmd)
  elseif t == "vol" then
    if msg.level ~= nil then DoVolumeSet(tonumber(msg.level))
    elseif msg.dir then DoVolumeStep(msg.dir) end
  elseif t == "mute" then DoMute()
  elseif t == "source" then DoSelectSource(tostring(msg.id))
  elseif t == "getrooms" then
    -- Both of these walk the project (GetDevicesByC4iName + a DeviceName per room)
    -- and are triggered BY THE DEVICE, with no natural limit on how often it may
    -- ask. On a controller whose Lua is single-threaded that is a free way for a
    -- chatty -- or hostile -- panel to pin the Director, so they are throttled.
    Send({ t = "rooms", list = QueryResult("rooms", BuildRoomsList) })
  elseif t == "grouproom" then DoGroupRoom(msg.id, msg.join)   -- multiroom join/leave (add-rooms tap)
  elseif t == "favorite" then DoPlayFavorite(msg)              -- play a favorite tile (by id; legacy mediaid ok)
  elseif t == "getfavorites" then
    -- room-level navigator favorites (agent 1609)
    Send({ t = "favorites", list = QueryResult("favorites", BuildFavoritesList) })
  elseif t == "artdbg" then
    -- Diagnostic only: which favourite tile fetched/decoded, and how big. Lets a
    -- "the artwork looks wrong" report be answered from the Director log instead
    -- of from a photograph.
    dbg("art tile", msg.slot, msg.ok and "PUBLISHED" or "ABANDONED/failed",
        tostring(msg.bytes) .. " bytes", tostring(msg.w) .. "x" .. tostring(msg.h))
  elseif t == "ping" then Send({ t = "pong" })
  elseif t == "button" then FireButton(msg.id)
  elseif t == "sipstate" or t == "callstate" or t == "callctl" then
    -- Intercom traffic → relay the line to the companion intercom driver (it owns the
    -- intercomproxy + SIP/call logic). Re-encode the decoded msg; the intercom driver
    -- parses it back with the same JSON contract.
    local okE, line = pcall(json.encode, msg)
    if okE then RelayToIntercom(line) end
  else dbg("ignoring unknown msg type:", tostring(t)) end
end

-- ============================================================================
-- Keypad proxy: programmable on-screen buttons with RGB LEDs (binding 5002)
-- ============================================================================
-- ---------------------------------------------------------------------------
-- BUTTON_LINK bindings (static 301-306 + runtime-created above that)
-- ---------------------------------------------------------------------------
-- Buttons used to be capped at 6 because driver.xml declares exactly six BUTTON_LINK
-- connections and static XML was treated as the ceiling. It isn't: Control4's own
-- room_control_keypad creates its BUTTON_LINK bindings at runtime with
-- C4:AddDynamicBinding(id, "CONTROL", true, name, "BUTTON_LINK", false, false)
-- rather than declaring a fixed set, so the count can follow the hardware.
--
-- Two rules make this safe:
--   1. The static 301-306 stay. Removing a <connection> from an installed driver
--      corrupts the project; adding is allowed. So 1..6 are XML, 7..N are dynamic,
--      and both live at BTN_LINK_BASE + i so every consumer stays plain arithmetic.
--   2. Dynamic bindings DO NOT survive a Director restart. The driver must re-add
--      every one it owns from persistence in OnDriverInit or, on the next controller
--      reboot, every link a dealer made to button 7+ is silently gone — the whole
--      binding, not just its connection. This is the single thing in here that must
--      not regress; C4's own driver does the same restore loop for the same reason.
--
-- KNOWN LIMITATION (control4/docs-driverworks#8): a binding added AFTER driver load is
-- created but comes up UNCONNECTED, and there is no documented API to connect it
-- programmatically. That is fine here — a dealer connects it on Composer's Connections
-- page like any other binding. Not solved here.
local BTN_BIND_KEY = "nv_btn_bindings"   -- persisted list of the binding ids we created

-- Highest button index that has ever had a binding in this install. InitKeypad uses it
-- to know how far up to send DELETE_KEYPAD_BUTTON when the count shrinks; a plain
-- ButtonCount() would leave orphaned buttons sitting in Composer's panel forever.
local gBtnHigh = KEYPAD_STATIC

local function SaveButtonBindings()
  local ids = {}
  for id in pairs(gBtnBindings) do ids[#ids + 1] = id end
  table.sort(ids)   -- stable, so the persisted string doesn't churn
  pcall(function() C4:PersistSetValue(BTN_BIND_KEY, table.concat(ids, ",")) end)
end

-- Create one BUTTON_LINK binding. Idempotent: re-adding a binding we already have is a
-- no-op, which matters because both the restore path and a hello can reach for the same
-- index. Returns true only if this call actually created it.
local function AddButtonBinding(i)
  local id = BTN_LINK_BASE + i
  if gBtnBindings[id] then return false end
  local name = "Button " .. i
  local ok = pcall(function()
    -- Same argument shape as room_control_keypad: CONTROL binding, provider=true,
    -- class BUTTON_LINK, not hidden, not auto-bind.
    C4:AddDynamicBinding(id, "CONTROL", true, name, "BUTTON_LINK", false, false)
  end)
  if not ok then dbg("AddDynamicBinding failed for binding", tostring(id)); return false end
  gBtnBindings[id] = name
  if i > gBtnHigh then gBtnHigh = i end
  return true
end

-- Bring the dynamic set up to the device's reported capacity. Called when a `hello`
-- manifest changes caps.buttons.
--
-- It only ever GROWS. When a device reports FEWER buttons than last time (a firmware
-- downgrade, or the driver moved to smaller hardware) the surplus bindings are left in
-- place: ButtonCount() already stops the driver using them, and tearing down a binding
-- a dealer may have wired is a destructive act to take on the strength of one manifest.
-- They cost nothing but a row on the Connections page, and they come back into use
-- verbatim if capacity returns.
function EnsureButtonBindings()
  local count = ButtonCount()
  local added = false
  for i = KEYPAD_STATIC + 1, count do
    if AddButtonBinding(i) then added = true end
  end
  if added then
    SaveButtonBindings()
    dbg("button bindings: now", tostring(count), "(", tostring(KEYPAD_STATIC), "static )")
  end
  return added
end

-- MANDATORY on every load: dynamic bindings are not persisted by Director, only the
-- fact that we made them (by us, here). Runs in OnDriverInit — the same place C4's
-- keypad driver restores its own, and before anything can reference a binding id.
function RestoreButtonBindings()
  local raw
  pcall(function() raw = C4:PersistGetValue(BTN_BIND_KEY) end)
  if type(raw) ~= "string" or raw == "" then return end
  local n = 0
  for s in raw:gmatch("[^,]+") do
    local id = tonumber(s)
    local i  = id and (id - BTN_LINK_BASE)
    if i and i > KEYPAD_STATIC and i <= KEYPAD_MAX then
      if AddButtonBinding(i) then n = n + 1 end
    end
  end
  -- The restored set is also the last capacity this device reported, so trust it until
  -- the next `hello` says otherwise. Without this the keypad collapses to 6 buttons for
  -- the seconds between a reload and the device reconnecting, and InitKeypad would
  -- helpfully DELETE the rest from Composer's panel on the way past.
  if gBtnHigh > gMaxButtons then gMaxButtons = gBtnHigh end
  print("NuVoxelKeypad: restored " .. n .. " dynamic button bindings")
end

-- Is this binding id one of ours? The static XML range is always ours; above it, only
-- what we actually created — an id we never added has no button behind it, so it must
-- fall through to the "ignoring proxy cmd" branch rather than index a nil button.
function IsButtonLinkBinding(id)
  id = tonumber(id); if not id then return false end
  if id > BTN_LINK_BASE and id <= BTN_LINK_BASE + KEYPAD_STATIC then return true end
  return gBtnBindings[id] ~= nil
end

-- Name of the device linked to button i's BUTTON_LINK binding (nil if unlinked). Lets a
-- linked button auto-title from its target. Tries both binding directions (our links are
-- consumer=True, but be defensive) and pcall-guards every C4 call.
function LinkedDeviceName(i)
  local me = myDeviceId()
  if not me or me == 0 then return nil end
  local binding = BTN_LINK_BASE + i
  local id
  local ok, prov = pcall(function() return C4:GetBoundProviderDevice(me, binding) end)
  if ok and tonumber(prov) and tonumber(prov) ~= 0 then id = tonumber(prov) end
  if not id then
    local ok2, cons = pcall(function() return C4:GetBoundConsumerDevices(me, binding) end)
    if ok2 and type(cons) == "table" then
      local k = next(cons)
      if tonumber(k) and tonumber(k) ~= 0 then id = tonumber(k) end
    end
  end
  if not id then return nil end
  local okn, name = pcall(function() return C4:GetDeviceDisplayName(id) end)
  name = okn and name and tostring(name) or ""
  return (name ~= "") and name or nil
end

-- ---------------------------------------------------------------------------
-- Button state
-- ---------------------------------------------------------------------------
-- gButtons IS the button configuration — there is no Composer property mirror of it
-- any more. It is edited from Composer's NATIVE keypad panel (KEYPAD_BUTTON_INFO /
-- KEYPAD_BUTTON_COLOR into HandleKeypadProxy) and pushed straight to the device.
--
-- PERSISTENCE: button config lives in the driver's own persisted state; it is not
-- mirrored into
-- C4:PersistSetValue: a local copy is a second source of truth that can only be
-- stale. The driver's own persisted state is authoritative.
local BTN_DEFAULT = { label = "", icon = "", color = "ffffff", tracks = true, on = false }

-- Grow/shrink gButtons to the device's button count WITHOUT disturbing entries that
-- already exist — this runs on every re-register, so it must never be the thing that
-- forgets a label.
function EnsureButtons()
  local count = ButtonCount()
  gButtons = gButtons or {}
  for i = 1, count do
    if type(gButtons[i]) ~= "table" then
      gButtons[i] = { id = i }
      for k, v in pairs(BTN_DEFAULT) do gButtons[i][k] = v end
    end
    gButtons[i].id = i
  end
  for i = #gButtons, count + 1, -1 do gButtons[i] = nil end
  return gButtons
end

local function btnField(i, key)
  local b = gButtons and gButtons[i]
  local v = b and b[key]
  if v == nil then return BTN_DEFAULT[key] end
  return v
end

-- On-screen label: the stored OVERRIDE, else the linked device's name (auto), else ""
-- (firmware then shows "Button N"). Label is purely an override — it never clobbers the
-- auto-name, which is the fallback.
function ButtonLabel(i)
  local l = tostring(btnField(i, "label") or "")
  if l ~= "" then return l end
  return LinkedDeviceName(i) or ""
end
-- Single per-button icon ("" for None). Shown in both states; the on/off (filled/highlight)
-- look comes from the LED state + button style, not a separate off-icon.
function ButtonIcon(i)
  local p = tostring(btnField(i, "icon") or "")
  return (p == "None") and "" or p
end

-- Colors reach us as rrggbb from the proxy; store one
-- canonical form so the two editors can be compared for equality at all.
function NormalizeHex(v)
  v = tostring(v or ""):gsub("^#", ""):lower()
  return v:match("^%x%x%x%x%x%x$") or "ffffff"
end

-- Per-button LED lit color as rrggbb hex. C4 only tells us on/off, so the keypad owns
-- the color; Programming can still override it live.
function ButtonColor(i)
  return NormalizeHex(btnField(i, "color"))
end
-- True = LED tracks the linked load (MATCH_LED_STATE); false = Manual (Programming only).
function ButtonTracks(i)
  return btnField(i, "tracks") ~= false
end

-- How many buttons this keypad has. The DEVICE decides, via its manifest caps.buttons
-- (gMaxButtons) — not a dealer-set count. There used to be a "Buttons" property layered
-- on top, and a stale value silently shrank a 6-button keypad to 5 (see the note on
-- gMaxButtons). Hardware capacity is the only honest answer.
--
-- The clamp is KEYPAD_MAX (a sanity bound on binding ids), NOT the six static XML
-- connections: those are the base set, not the ceiling — bindings past them are created
-- at runtime. A 10" panel reporting caps.buttons = 12 gets twelve buttons.
function ButtonCount()
  return math.max(0, math.min(KEYPAD_MAX, math.floor(gMaxButtons or KEYPAD_STATIC)))
end

-- gButtons is the driver's config record and carries fields the device has no business
-- seeing (the LED tracking mode is resolved here, not on the keypad). The `buttons`
-- array in PROTOCOL.md is a fixed shape, so project onto exactly that — same keys, same
-- resolved label, as before this refactor.
-- Only CONFIGURED buttons go to the device. A keypad reports how many buttons it can
-- show (caps.buttons), which is an "up to" -- it is not a claim that six or eight
-- things are set up. Sending them all meant the room page rendered a tile for every
-- unconfigured slot: a grid of blanks (the firmware fills the name in as "Button N")
-- that do nothing when tapped. Nothing configured now means no button tiles at all,
-- and a button appears the moment it is given a label or linked to a device.
--
-- `id` stays the REAL button id, not a compacted index: the device echoes it back on
-- a press, and renumbering would fire the wrong button as soon as one slot is skipped.
function ButtonsForWire()
  local out = {}
  for i = 1, ButtonCount() do
    local label = ButtonLabel(i)       -- override, else linked device name, else ""
    if label ~= "" then
      out[#out + 1] = {
        id = i,
        label = label,
        icon = ButtonIcon(i), offIcon = "",   -- one icon; on/off look = LED state
        color = ButtonColor(i),          -- lit color = LED color
        on = btnField(i, "on") == true,
      }
    end
  end
  return out
end

-- Show only the settings relevant to THIS device, from its manifest caps
-- (SetPropertyAttribs: 0 = show, 1 = hide). Hardware without touch has no keypad
-- buttons or on-screen chrome; without an RGB LED, no Halo; a fixed-orientation
-- panel has no Orientation; no intercom hardware, no intercom rows. Called on
-- every hello (manifest may change across a firmware update).
function ApplyManifestPropVisibility(mf)
  local caps = (type(mf) == "table" and type(mf.caps) == "table") and mf.caps or {}
  local function show(name, on)
    pcall(function() C4:SetPropertyAttribs(name, on and 0 or 1) end)
  end
  local hasTouch = caps.touch == true
  local hasLed   = caps.led == true
  local canRot   = caps.rotate == true
  -- Intercom support can be advertised either in the manifest caps or as a bare
  -- `intercom` flag on the hello (older firmware); gDeviceIntercom already holds
  -- whichever arrived, so trust it rather than re-reading only one of the two.
  local hasInter = (caps.intercom == true) or gDeviceIntercom
  -- Max buttons the device can show ("up to N"); default from touch when absent.
  local maxBtn = tonumber(caps.buttons) or (hasTouch and KEYPAD_STATIC or 0)
  local hasButtons = maxBtn > 0

  -- Keypad buttons: the per-button config properties are gone (Composer's native
  -- keypad panel is the editor), so all that is left to gate here is the section
  -- header and the shared button style.
  show("Keypad Buttons", hasButtons)

  -- On-screen views / chrome: touch devices only.
  for _, n in ipairs({ "Layout", "Background", "Now-Playing", "Show Title",
                       "Show Artist/Album", "Show Info Button", "Show Progress Bar" }) do
    show(n, hasTouch)
  end

  show("Display Orientation", canRot)
  for _, n in ipairs({ "Halo", "Halo Color", "Halo Ring Color", "Halo Brightness" }) do show(n, hasLed) end
  -- Intercom: install trigger + relay diagnostic — only on intercom-capable devices.
  -- Intercom rows: only on intercom-capable hardware. The intercomproxy sub-proxy
  -- itself always exists (proxies are fixed at install time and cannot be added or
  -- removed later without corrupting the project) — we just hide its UI.
  for _, n in ipairs({ "Intercom", "SIP Registration", "Intercom Status" }) do show(n, hasInter) end
end

-- What we last registered on the proxy. The re-register is idempotent but NOT free —
-- it repaints Composer's Assembled Keypad — so it is skipped when nothing it carries
-- has changed. nil at load, so the first run after a driver reload always registers.
local gKpPrint = nil

-- Drop the fingerprint after someone changed gButtons behind InitKeypad's back (the
-- native panel telling us what IT already applied). Without this the cached print would
-- claim a value the proxy no longer holds, and a later edit back to that value would be
-- skipped as "unchanged" — the keypad would silently ignore it.
function gKpPrintInvalidate() gKpPrint = nil end

-- Register the buttons on the keypad proxy. Returns true if it actually (re)registered.
function InitKeypad()
  local count = ButtonCount()
  EnsureButtons()

  -- Fingerprint exactly the fields NEW_KEYPAD_BUTTON carries. Deliberately excludes the
  -- LED on/off state: that changes constantly with the linked load and is pushed to the
  -- device, not to the proxy — including it would re-register the whole keypad every
  -- time a light turned on.
  local parts = { tostring(count) }
  for i = 1, count do
    parts[#parts + 1] = table.concat({ i, ButtonLabel(i), ButtonIcon(i), ButtonColor(i) }, "\1")
  end
  local print_ = table.concat(parts, "\2")
  if print_ == gKpPrint then
    dbg("keypad re-init skipped (unchanged,", count, "buttons)")
    return false
  end
  gKpPrint = print_

  for i = 1, count do
    local label = ButtonLabel(i)
    local proxyName = (label ~= "") and label or ("Button " .. i)  -- programming needs a name
    -- ON_COLOR used to read a bare `color`, which is not a variable in this scope —
    -- Lua evaluated it to nil and silently dropped the key from the table. Never
    -- caught because <combo> meant the keypad proxy was never instantiated, so
    -- nothing was listening to this call in the first place.
    local onColor = ButtonColor(i)
    if not onColor or onColor == "" then onColor = "ffffff" end
    pcall(function()
      C4:SendToProxy(KEYPAD_BINDING, "NEW_KEYPAD_BUTTON", {
        BUTTON_ID = i, NAME = proxyName, ENGRAVING = proxyName,
        ON_COLOR = onColor, OFF_COLOR = "000000", SLOTS = 1, LOCK_COLORS = false,
      }, "NOTIFY")
    end)
  end
  -- Clear anything above the current count, up to the highest index this install has
  -- ever had (gBtnHigh), so shrinking really does empty Composer's panel. Bounded by a
  -- watermark rather than a constant now that the count is hardware-driven.
  for i = count + 1, math.max(gBtnHigh, KEYPAD_STATIC) do
    pcall(function() C4:SendToProxy(KEYPAD_BINDING, "DELETE_KEYPAD_BUTTON", { num = i }, "NOTIFY") end)
  end
  pcall(function() C4:SendToProxy(KEYPAD_BINDING, "ONLINE_CHANGED", { STATE = true }, "NOTIFY") end)
  return true
end

-- Coalesce button edits into one re-register + push.
--
-- The 400ms timer alone did not do it: the triggers are not a burst. A live trace showed
-- four InitKeypad runs across ten seconds — LateInit, then the BUTTON_LINK bindings
-- settling, then further init triggers arriving seconds apart. No
-- debounce window covers that without also making a genuine edit feel broken. So the
-- debounce only handles same-tick storms and InitKeypad's fingerprint handles the rest:
-- a redundant trigger now costs one string compare. EVERY caller goes through here
-- (including LateInit) so there is exactly one path to the proxy.
function ScheduleInitKeypad()
  cancelTimer(gKpTimer)
  gKpTimer = C4:SetTimer(400, function()
    gKpTimer = nil
    local changed = InitKeypad()
    -- Only force a full state push when something actually moved; otherwise let the
    -- normal dedupe in PushState decide, so a no-op trigger costs nothing on the wire.
    if gConnected then pcall(PushState, changed) end
  end, false)
end

-- Every proxy/binding message arrives here and is dispatched on the BINDING ID. This
-- is what makes one multi-proxy driver work where we previously needed two drivers:
--   5002  keypad proxy        -> button colors / engravings / state
--   5003  intercomproxy       -> the whole SIP endpoint (intercom.lua)
--   301+  BUTTON_LINK control -> a linked driver actuating one of our buttons
function ReceivedFromProxy(idBinding, strCommand, tParams)
  tParams = tParams or {}
  -- The agent's SendToDevice(MAC) may land here rather than ExecuteCommand.
  if strCommand == "NV_SET_TARGET_MAC" and tParams.mac then
    SetTargetMac(tParams.mac, "agent handoff (proxy)"); return
  end
  if strCommand == "NV_SET_TARGET_IP" and tParams.ip then
    SetTargetIp(tParams.ip, "agent handoff (proxy)"); return
  end
  if idBinding == KEYPAD_BINDING then
    HandleKeypadProxy(strCommand, tParams)
  -- Static 301-306 plus whatever we created dynamically. Following the dynamic set (not
  -- a constant) is what lets button 7+ actually reach HandleButtonLink.
  elseif IsButtonLinkBinding(idBinding) then
    HandleButtonLink(idBinding - BTN_LINK_BASE, idBinding, strCommand, tParams)
  else
    dbg("ignoring proxy cmd on binding", tostring(idBinding), tostring(strCommand))
  end
end

-- A linked driver actuates / queries one of our buttons over its BUTTON_LINK binding.
function HandleButtonLink(n, idBinding, cmd, p)
  local b = gButtons and gButtons[n]; if not b then return end
  if cmd == "REQUEST_BUTTON_COLORS" then
    local on = (b.color and b.color ~= "000000") and b.color or "00ff00"
    pcall(function() C4:SendToProxy(idBinding, "BUTTON_COLORS", { ON_COLOR = on, OFF_COLOR = "000000" }, "NOTIFY") end)
    pcall(function() C4:SendToProxy(idBinding, "MATCH_LED_STATE", { STATE = b.on }, "NOTIFY") end)
  elseif cmd == "SET_BUTTON_COLOR" then
    local c = p.CURRENT_COLOR or p.ON_COLOR or p.COLOR
    if c then b.color = tostring(c) end
    if p.STATE ~= nil then b.on = (p.STATE == true) or (tostring(p.STATE) == "true") end
    PushState(true)
  elseif cmd == "MATCH_LED_STATE" then
    -- Control4 tracks the linked load and tells us on/off; we own the color (the
    -- per-button "Button N Color" property). Honor the per-button tracking toggle:
    -- in Manual mode the LED is driven only by Programming, so ignore tracking.
    if ButtonTracks(n) then
      local s = tostring(p.STATE):lower()
      b.on = (p.STATE == true) or s == "true" or s == "1" or s == "on"
      PushState(true)
    else
      dbg("button", n, "LED Manual mode; ignoring MATCH_LED_STATE")
    end
  elseif cmd == "DO_PUSH" or cmd == "DO_CLICK" or cmd == "DO_RELEASE" then
    dbg("link", tostring(cmd), "button", n)
  else
    dbg("ignoring link cmd:", tostring(cmd))
  end
end

-- Composer's NATIVE keypad editor -> us: the dealer set an LED color / engraving /
-- state on a button. It is the editor of gButtons, so an edit here is pushed to the
-- device.
function HandleKeypadProxy(strCommand, tParams)
  if not gButtons then return end
  local function btn(id) id = tonumber(id); return id and gButtons[id] end
  if strCommand == "KEYPAD_BUTTON_INFO" or strCommand == "KEYPAD_BUTTON_COLOR" then
    local b = btn(tParams.BUTTON_ID); if not b then return end
    local on = (tParams.STATE == true) or (tostring(tParams.STATE) == "true")
    local wasOn, wasColor, wasLabel = b.on, b.color, b.label
    b.on = on
    local cur = tParams.CURRENT_COLOR or (on and tParams.ON_COLOR or tParams.OFF_COLOR)
    if cur then b.color = NormalizeHex(cur) end
    -- The engraving we sent for an unlabelled button is the auto-name (linked device, or
    -- "Button N"). Echoing that straight back would harden a fallback into a permanent
    -- override, so only store an engraving that is genuinely something else.
    local lbl = tParams.ENGRAVING or tParams.NAME
    if lbl then
      lbl = tostring(lbl)
      local auto = ButtonLabel(tonumber(tParams.BUTTON_ID))
      if auto == "" then auto = "Button " .. tostring(tParams.BUTTON_ID) end
      if lbl ~= "" and lbl ~= auto then b.label = lbl end
    end
    PushState(true)
    -- Only the LED on/off changed -> that is live load state, not saved configuration.
    if b.color ~= wasColor or b.label ~= wasLabel then
      gKpPrintInvalidate()
    elseif b.on ~= wasOn then
      dbg("button", tostring(tParams.BUTTON_ID), "LED ->", tostring(b.on))
    end
  elseif strCommand == "KEYPAD_ALL_BUTTON_COLOR" then
    if tParams.CURRENT_COLOR then
      for _, b in ipairs(gButtons) do b.color = NormalizeHex(tParams.CURRENT_COLOR) end
      PushState(true)
      gKpPrintInvalidate()
    end
  else
    dbg("ignoring proxy cmd:", tostring(strCommand))
  end
end

-- Firmware reported a button tap -> raise the programmable Push/Release events.
function FireButton(n)
  n = tonumber(n); if not n then return end
  -- M7: clamp to the configured count so a stray/out-of-range id from the device
  -- can't actuate a hidden link binding the installer can't see.
  if n < 1 or n > ButtonCount() then dbg("button", tostring(n), "out of range; ignoring"); return end
  -- ButtonCount() is the hardware's CAPACITY, not what is set up. Only a configured
  -- button may fire programming: an unconfigured slot is not shown on the panel, so
  -- a press for one can only come from a device that made it up.
  if ButtonLabel(n) == "" then dbg("button", tostring(n), "not configured; ignoring"); return end
  dbg("button", n, "-> keypad proxy + link")
  -- programmable event (Programming tab)
  pcall(function() C4:SendToProxy(KEYPAD_BINDING, "KEYPAD_BUTTON_ACTION", { BUTTON_ID = n, ACTION = 1 }, "NOTIFY") end)
  pcall(function() C4:SendToProxy(KEYPAD_BINDING, "KEYPAD_BUTTON_ACTION", { BUTTON_ID = n, ACTION = 0 }, "NOTIFY") end)
  -- driver-owned programming events (reliably listed under the device in Programming,
  -- independent of the keypad proxy). A touchscreen tap is press-then-release.
  pcall(function() C4:FireEvent("Button " .. n .. " Pressed") end)
  pcall(function() C4:FireEvent("Button " .. n .. " Released") end)
  -- drive whatever is linked to this button in Connections (no-op if unlinked)
  pcall(function() C4:SendToProxy(BTN_LINK_BASE + n, "DO_CLICK", {}, "NOTIFY") end)
end

-- ============================================================================
-- Room state read + push
-- ============================================================================
local function getRoomVar(roomId, varId)
  if not roomId then return nil end
  local ok, val = pcall(C4.GetDeviceVariable, C4, tonumber(roomId), varId)
  if ok then return val end
  return nil
end

-- Display Orientation property -> int the device understands (-1 = Auto, let the
-- device's own/web setting stand).
local ORIENT_MAP = {
  ["Landscape"] = 0, ["Portrait"] = 1,
  ["Landscape (flipped)"] = 2, ["Portrait (flipped)"] = 3,
}
function OrientationInt()
  return ORIENT_MAP[Properties and Properties["Display Orientation"]] or -1
end

-- Layout property -> int (-1 = Auto, leave the device/web setting).
local LAYOUT_MAP = {
  ["Cover (fill screen)"] = 0, ["Fit (whole art)"] = 1, ["Compact (no overlap)"] = 2,
}
function LayoutInt()
  return LAYOUT_MAP[Properties and Properties["Layout"]] or -1
end


-- Background gradient preset -> int (-1 = Auto, leave the device/web setting).
-- The room's actual Navigator wallpaper isn't exposed to drivers, so this picks a
-- gradient preset on the device instead.
local BG_MAP = { ["Navigator"] = 0, ["Ocean"] = 1, ["Dusk"] = 2, ["Graphite"] = 3 }
function BackgroundInt()
  return BG_MAP[Properties and Properties["Background"]] or -1
end

-- Active screen brightness % -> int (-1 = Auto, leave the device/web setting).
local BRIGHT_MAP = { ["10%"] = 10, ["25%"] = 25, ["50%"] = 50, ["75%"] = 75, ["100%"] = 100 }
function BrightnessInt()
  return BRIGHT_MAP[Properties and Properties["Active Brightness"]] or -1
end

-- Idle dim timeout -> seconds (-1 = Auto/leave; "Never" = 0 = never dim).
local DIMSEC_MAP = {
  ["Never"] = 0, ["15 seconds"] = 15, ["30 seconds"] = 30,
  ["1 minute"] = 60, ["2 minutes"] = 120, ["5 minutes"] = 300, ["10 minutes"] = 600,
}
function DimSecInt()
  local v = Properties and Properties["Idle Timeout"]
  if v == nil or v == "Auto (device setting)" then return -1 end
  return DIMSEC_MAP[v] or -1
end

-- Idle backlight % after the timeout -> int (-1 = Auto/leave; "Screen off" = 0 = dark,
-- a touch wakes it).
local DIMLVL_MAP = { ["Screen off"] = 0, ["5%"] = 5, ["10%"] = 10, ["25%"] = 25, ["50%"] = 50 }
function DimLevelInt()
  local v = Properties and Properties["Idle Brightness"]
  if v == nil or v == "Auto (device setting)" then return -1 end
  return DIMLVL_MAP[v] or -1
end

-- Now-playing element visibility properties. Default (and anything but "Hide") = show.
function ShowProp(name)
  return (Properties and Properties[name]) ~= "Hide"
end

-- ============================================================================
-- Settings
-- ----------------------------------------------------------------------------
-- The driver's Composer properties are the single source of truth for the
-- configuration below (orientation, layout, brightness, idle behaviour, halo).
-- They are applied to the device over the :6700 protocol on connect and whenever
-- a property changes (OnPropertyChanged -> PushState / PushHalo).
--
-- "Auto (device setting)" maps to the -1 sentinel the firmware's range guards
-- skip, leaving the device's own NVS value in place. The device protocol is
-- unchanged (see PROTOCOL.md).



-- Stable key order so an unchanged push is byte-identical (cheap change detect).
SETTING_ORDER = {
  "orientation", "layout", "bg",
  "brightness", "dimSec", "dimLevel",
  "showTitle", "showArtist", "showInfo", "showProgress",
  "haloColor", "haloRingColor", "haloBrightness",
  "buttonCount", "buttons",
}

local AUTO = "Auto (device setting)"












-- Read the real transport state for a room from the media service's QUEUE_STATUS_V2
-- (<state>Play|Pause|Stop</state>) — the reliable external-pause signal. svcId is
-- the media-service deviceid carried in the room's CURRENT_MEDIA_INFO while playing.
-- Live queue truth for a room, from the media session device (the digital-audio
-- device carried as <deviceid> in the room's media info). Returns nil when this
-- room has no queue on that device.
--
--   QUEUE_STATUS_V2 (var 1006) per queue: <owner> room, <state> Play|Pause|Stop,
--                                         <shuffle> <repeat> <index>
--   QUEUE_INFO_V2   (var 1007)          : <songs><song>… for the CURRENT queue
--
-- This is what Navigator itself drives its transport row from, and it is the reason
-- the driver no longer guesses with an "isRadio" heuristic: the <Dashboard> says what
-- a SOURCE can ever do (a static superset -- Apple Music declares thumbs, shuffle,
-- repeat and skip-rev at all times), while THIS says what applies to what is playing
-- right now. Guessing from the stream description was provably wrong: Apple reports
-- audioquality=Radio as a BITRATE TIER on ordinary album tracks, and carries a
-- <stationid> even on navigable content.
function QueueLive(svcId, roomId)
  local id = tonumber(svcId)
  if not id then return nil end
  local ok, xml = pcall(function() return C4:GetDeviceVariable(id, 1006) end)
  if not ok or not xml or xml == "" then return nil end
  local q
  for blk in tostring(xml):gmatch("<queue>(.-)</queue>") do
    if blk:match("<owner>(%d+)</owner>") == tostring(roomId)
       or blk:find("<id>" .. tostring(roomId) .. "</id>") then q = blk; break end
  end
  if not q then return nil end
  local out = {
    state   = q:match("<state>(%a+)</state>"),
    shuffle = q:match("<shuffle>(%d+)</shuffle>") == "1",
    repeatt = tonumber(q:match("<repeat>(%d+)</repeat>") or "0") or 0,
    index   = tonumber(q:match("<index>(%d+)</index>") or "0") or 0,
    qid     = q:match("<id>(%d+)</id>"),
    count   = 0,
  }
  -- Queue LENGTH decides what is navigable. A station is a single-item queue: you can
  -- skip forward (the station serves the next track) but there is nothing to skip back
  -- to and nothing to shuffle or repeat. A real album/playlist queue has many.
  local ok2, info = pcall(function() return C4:GetDeviceVariable(id, 1007) end)
  if ok2 and info and info ~= "" then
    for _ in tostring(info):gmatch("<song[%s>]") do out.count = out.count + 1 end
  end
  return out
end

-- Back-compat shim: the play/pause path only ever wanted <state>.
-- Track our room's current media-session device (the <deviceid> in room 1031) so
-- media-session events for OTHER rooms' sessions can be dropped before any work.
function updateSessionDev(raw)
  gRoomSessionDev = tostring(raw or ""):match("<deviceid>(%d+)</deviceid>") or ""
  if gRoomSessionDev ~= "" then gAudioDev = gRoomSessionDev end
end

-- Multiroom "add-rooms" list: every audio room, which are grouped with us (share our
-- session's queue), and what each is doing. Built ON DEMAND (device asks when the panel
-- opens) — not a hot path. One room enumeration + one read of var 1006 (all queues).
function BuildRoomsList()
  local rid = tonumber(gRoom)
  -- 1) all audio rooms: id -> name
  local names = {}
  local ok, list = pcall(function() return C4:GetDevicesByC4iName("roomdevice.c4i") end)
  if ok and type(list) == "table" then
    for k, v in pairs(list) do
      local id = (type(v) == "string") and tonumber(k) or tonumber(v)
      if id and id > 0 then
        local nm = (type(v) == "string") and v or DeviceName(id)
        names[id] = (nm and nm ~= "") and nm or tostring(id)
      end
    end
  end
  -- 2) all queues (house-wide) from var 1006: room -> play-state, and our group set
  local state, ourGroup = {}, {}
  local dev = (gRoomSessionDev and gRoomSessionDev ~= "" and gRoomSessionDev) or gAudioDev
  if not dev then   -- not learned from an event yet: find the digital-audio device by name
    local okd, dl = pcall(function() return C4:GetDevicesByC4iName("control4_digitalaudio.c4i") end)
    if okd and type(dl) == "table" then
      for k, v in pairs(dl) do dev = (type(v) == "string") and k or v; break end
    end
  end
  if dev then
    local okq, xml = pcall(function() return C4:GetDeviceVariable(tonumber(dev), 1006) end)
    if okq and xml then
      for q in tostring(xml):gmatch("<queue>(.-)</queue>") do
        local st = q:match("<state>(%a+)</state>") or ""
        local block = q:match("<rooms>(.-)</rooms>") or ""
        local members, mine = {}, false
        for m in block:gmatch("<id>(%d+)</id>") do
          local mid = tonumber(m); members[#members + 1] = mid
          if mid == rid then mine = true end
        end
        for _, mid in ipairs(members) do
          state[mid] = st
          if mine then ourGroup[mid] = true end
        end
      end
    end
  end
  -- 3) assemble
  local out = {}
  for id, nm in pairs(names) do
    out[#out + 1] = {
      id      = tostring(id),
      name    = Normalize(nm),
      grouped = (ourGroup[id] == true) or (id == rid),   -- our own room is always "in"
      playing = (state[id] == "Play"),
      active  = state[id] ~= nil and state[id] ~= "",     -- in some session
    }
  end
  table.sort(out, function(a, b)
    if a.grouped ~= b.grouped then return a.grouped end   -- grouped first
    return a.name:lower() < b.name:lower()
  end)
  return out
end

function BuildState(roomId)
  if not roomId then return nil end
  local power  = getRoomVar(roomId, ROOM_VAR.POWER_STATE)
  local vol    = tonumber(getRoomVar(roomId, ROOM_VAR.CURRENT_VOLUME))
  local muted  = getRoomVar(roomId, ROOM_VAR.IS_MUTED)
  local selDev = getRoomVar(roomId, ROOM_VAR.CURRENT_SELECTED_DEVICE)
  local vidDev = getRoomVar(roomId, ROOM_VAR.CURRENT_VIDEO_DEVICE)
  local audDev = getRoomVar(roomId, ROOM_VAR.CURRENT_AUDIO_DEVICE)
  local plyDev = getRoomVar(roomId, ROOM_VAR.PLAYING_AUDIO_DEVICE)
  local roomMediaRaw = getRoomVar(roomId, ROOM_VAR.CURRENT_MEDIA_INFO)
  updateSessionDev(roomMediaRaw)   -- keep the media-event early-out key current
  local media  = ParseMediaInfo(roomMediaRaw)
  -- Follow the digital-audio aggregator (the <deviceid> in room 1031) with a var watch
  -- so its shuffle/repeat/queue-state changes push to the device instantly (event-driven).
  local aggId = tostring(roomMediaRaw or ""):match("<deviceid>(%d+)</deviceid>")
  if aggId then WatchAggregator(aggId) end

  local function valid(d) local s = tostring(d or ""); return (s ~= "" and s ~= "0") and s or nil end
  -- When something is playing, prefer the actual streaming source (medSrcDev /
  -- PLAYING_AUDIO_DEVICE). When idle, PLAYING_AUDIO_DEVICE is stale (a previous
  -- endpoint, e.g. "Apple Music"), so prefer the room's SELECTED source so the source tile
  -- shows what the user picked (e.g. "Apple Mac") rather than the stale device.
  local srcId
  if media.title ~= "" then
    srcId = valid(media.srcDev) or valid(plyDev) or valid(audDev) or valid(vidDev) or valid(selDev) or ""
  else
    srcId = valid(selDev) or valid(audDev) or valid(vidDev) or valid(plyDev) or ""
  end

  -- Room 1031 carries the now-playing for most audio sources (verified: Apple Music
  -- puts title/artist/streamStatus on the ROOM, with an empty source-device 1031).
  -- Only fall back to the source device / a variable scan when the room is sparse
  -- (e.g. video). [M6 external-pause handling lives in DoTransport/gLastPlaying.]
  local appName = ""
  if media.title == "" and srcId ~= "" then
    local devMedia = ParseMediaInfo(getRoomVar(srcId, ROOM_VAR.CURRENT_MEDIA_INFO))
    if devMedia.title ~= "" then media = devMedia end
    if media.title == "" then
      local mediaXml, app = ScanDeviceMedia(srcId)
      if mediaXml then
        local dm = ParseMediaInfo(mediaXml)
        if dm.title ~= "" then media = dm end
      end
      if app and app ~= "" then appName = app end
    end
  end

  -- M6: authoritative play/pause. The room's streamStatus stays OK_playing even
  -- when paused in Navigator, but the media service's QUEUE_STATUS_V2 <state>
  -- tracks Play/Pause/Stop for this room (verified live: Apple Music + Pandora).
  local qsvcId = tostring(roomMediaRaw or ""):match("<deviceid>(%d+)</deviceid>")
  local qlive  = qsvcId and QueueLive(qsvcId, roomId) or nil
  if media.title ~= "" then
    local svcId = qsvcId
    local qstate = qlive and qlive.state
    if qstate then
      media.playing = (qstate == "Play")
    elseif svcId then
      -- Digital-audio session device present but NO queue entry for this room: the
      -- room's CURRENT MEDIA INFO is STALE, left over from a previous session. The
      -- room variable keeps the last track forever, so the panel showed a phantom
      -- "now playing" (e.g. a track from hours ago) with transport controls that do
      -- nothing. Control4's own client treats this state as empty -- it shows
      -- "0 Tracks" and hides its transport entirely. Report nothing playing so the
      -- panel agrees with the app instead of lying about the state.
      -- Only when svcId is present: a plain AV source (tuner, etc.) has no queue at
      -- all and must keep its metadata.
      dbg("stale room media (session dev", svcId, "has no queue for room", roomId, ") -> clearing")
      media.title, media.artist, media.album, media.artUrl = "", "", "", ""
      media.playing  = false
      media.duration = 0
      media.elapsed  = 0
    end
  end

  -- Source label: a reported "current app" (e.g. YouTube) wins over the device name.
  local srcName = Normalize((appName ~= "" and appName) or DeviceName(srcId) or "")

  local artUrl = media.artUrl
  if artUrl == "" and srcId ~= "" then artUrl = DeviceIcon(srcId) end
  -- Request a small image where the CDN encodes size in the URL — a 1024px JPEG
  -- is slow/unreliable to fetch on the device. Apple (mzstatic) + Pandora (p-cdn).
  -- ~600px so it can fill the device screen (cover-cropped) yet stay a fast fetch.
  if artUrl ~= "" then
    artUrl = artUrl:gsub("%d+x%d+bb%.jpg", "600x600bb.jpg")       -- Apple
    artUrl = artUrl:gsub("_%d+W_%d+H%.jpg", "_600W_600H.jpg")     -- Pandora
  end

  -- Layer 1: transport capability flags from the source's <Dashboard>. When a source
  -- declares none (no Dashboard), default to full play/pause + skip so ordinary
  -- on-demand sources behave exactly as before.
  local caps = (srcId ~= "") and GetSourceCaps(srcId) or { found = false }
  local function capOr(v, def) if caps.found then return v == true else return def end end
  -- Remember the source's thumb commands (PROTOCOL -> sent to the source device).
  gThumbUp   = (caps.thumbsUp   and caps.thumbUpCmd)   and { dev = srcId, cmd = caps.thumbUpCmd }   or nil
  gThumbDown = (caps.thumbsDown and caps.thumbDownCmd) and { dev = srcId, cmd = caps.thumbDownCmd } or nil
  gShuffle   = (caps.shuffle    and caps.shuffleCmd)   and { dev = srcId, cmd = caps.shuffleCmd }   or nil
  gRepeat    = (caps.repeatt    and caps.repeatCmd)    and { dev = srcId, cmd = caps.repeatCmd }    or nil

  -- Real shuffle/repeat STATE: the digital-audio session device (the <deviceid> in
  -- room 1031, e.g. 100002 control4_digitalaudio) exposes per-room settings in var
  -- 1003 (room_queue_settings). Var 1006 lags/caches; 1003 + 1009 track live, so use
  -- 1003 keyed by our roomid. Only read it when the source actually has shuffle/repeat.
  local shuffleOn, repeatOn = false, false
  if caps.found and (caps.shuffle or caps.repeatt) then
    local daId = tostring(roomMediaRaw or ""):match("<deviceid>(%d+)</deviceid>")
    if daId then
      local ok, qs = pcall(function() return C4:GetDeviceVariable(tonumber(daId), 1003) end)
      if ok and qs then
        for blk in tostring(qs):gmatch("<room_info>(.-)</room_info>") do
          if blk:match("<roomid>(%d+)</roomid>") == tostring(roomId) then
            shuffleOn = (blk:match("<shuffle>(%d+)</shuffle>") == "1")
            repeatOn  = (blk:match("<repeat>(%d+)</repeat>")  == "1")
            break
          end
        end
      end
    end
  end

  local powerOn = (tostring(power) == "1" or tostring(power):lower() == "true")
  local hasMedia = (media.title ~= "" or media.artist ~= "" or media.album ~= "")

  -- Poll rate follows power AND playback (see StartPolling): re-rate the moment
  -- either flips rather than waiting for the next event transition.
  local wasPlaying = gLastPlaying
  gLastPlaying = media.playing or false   -- M6: reused by the playpause command
  if powerOn ~= gLastPower or gLastPlaying ~= wasPlaying then
    gLastPower = powerOn
    if not gEventDriven then StartPolling() end
  end
  -- Send the room's DISPLAY NAME (e.g. "Office"), not its numeric id — the device
  -- shows this as "Playing in <room>". Fall back to the id if the name is unavailable.
  local roomName = DeviceName(roomId)
  if roomName == "" then roomName = tostring(roomId) end
  return {
    t         = "state",
    proto     = PROTO_VERSION,   -- M1: version is now actually sent (was dead)
    driverVersion = DRIVER_VERSION,  -- shown on the device's settings page
    room      = roomName,
    power     = powerOn,
    playing   = media.playing or false,
    mediaType = hasMedia and "media" or (powerOn and "other" or "off"),
    mediaTypeV2 = media.mediaTypeV2 or "",
    title     = media.title or "",
    artist    = media.artist or "",
    album     = media.album or "",
    artUrl    = artUrl or "",
    source    = { id = srcId, name = srcName },
    volume    = vol or 0,
    muted     = (tostring(muted) == "1" or tostring(muted):lower() == "true"),
    duration  = media.duration or 0,
    position  = media.position or 0,
    meta      = media.meta or {},
    rotation  = OrientationInt(),
    layout    = LayoutInt(),
    bg        = BackgroundInt(),
    brightness = BrightnessInt(),
    dimSec    = DimSecInt(),
    dimLevel  = DimLevelInt(),
    showTitle    = ShowProp("Show Title"),
    showArtist   = ShowProp("Show Artist/Album"),
    showInfo     = ShowProp("Show Info Button"),
    showProgress = ShowProp("Show Progress Bar"),
    -- Two authoritative layers, no heuristics:
    --   <Dashboard>       -> what this SOURCE can ever do (static superset)
    --   QUEUE_STATUS/INFO -> what applies to what is playing NOW (see QueueLive)
    -- A single-item queue is a station: skip-forward works (the station serves the
    -- next track) but there is nothing to skip BACK to, and nothing to shuffle or
    -- repeat. A multi-item queue is an album/playlist and everything applies.
    canPause      = capOr(caps.pause, true),
    canStop       = capOr(caps.stop, false),
    canNext       = capOr(caps.nextt, true),
    canPrev       = capOr(caps.prev, true),
    -- Thumbs: Dashboard-declared like the rest. Nothing in the queue data marks an
    -- item as rateable, and there is no DashboardChanged system event to ask -- of the
    -- 132 events this Director exposes, none relate to Dashboard/capability/transport
    -- (enumerated live), so the old "unreadable DashboardChanged" note was wrong: it
    -- does not exist at all. Apple Music therefore shows thumbs where Navigator hides
    -- them. Declared-but-inapplicable, not guessed-wrong.
    canThumbsUp   = caps.found and caps.thumbsUp or false,
    canThumbsDown = caps.found and caps.thumbsDown or false,
    -- Shuffle/repeat are QUEUE MODES, not per-item capabilities: repeat-one on a
    -- single track is legitimate. So they follow the source's declaration and are not
    -- masked by queue shape.
    canShuffle    = caps.found and caps.shuffle or false,
    canRepeat     = caps.found and caps.repeatt or false,
    -- Live toggle state straight off the queue when we have it (authoritative), else
    -- the older ROOM_QUEUE_SETTINGS read.
    shuffleOn     = (qlive ~= nil) and qlive.shuffle or shuffleOn,
    repeatOn      = (qlive ~= nil) and (qlive.repeatt ~= 0) or repeatOn,
    buttons   = ButtonsForWire(),
  }
end

function PushState(force)
  if not gConnected or not gRoom then return end
  local state = BuildState(gRoom)
  if not state then return end
  local encoded = json.encode(state)
  if force or encoded ~= gLastState then
    gLastState = encoded
    C4:SendToNetwork(BINDING_NET, DEVICE_PORT, encoded .. "\n")
  end
end

-- Coalesce refresh triggers. Many callbacks (every watched-var change, every
-- MediaSession event) each used to run the full BuildState synchronously. While
-- audio is streaming those fire in a tight burst — the room's media-info var and
-- the aggregator's queue vars tick continuously, and system events are global so
-- EVERY active room's stream reaches EVERY instance — which pinned the Director's
-- single Lua thread and lagged the whole system. Collapse a burst into at most one
-- BuildState per PUSH_COALESCE_MS. PushState still dedupes the wire send, so this
-- only bounds the *rate* of the expensive build; it never drops a real change (the
-- trailing edge always runs). Immediate/forced pushes still call PushState directly.
function SchedulePush()
  if gPushPending then return end
  gPushPending = C4:SetTimer(PUSH_COALESCE_MS, function()
    gPushPending = nil
    if gConnected and gRoom then pcall(PushState, false) end
  end, false)
end

function StartPolling()
  StopPolling()
  -- Effective interval: fast (gPollMs) by default, but the slow heartbeat once
  -- MediaSession events are proven to be arriving (gEventDriven). OnMediaSessionEvent
  -- flips gEventDriven and re-calls StartPolling to switch rates live.
  -- The fast rate is for a room that is actively PLAYING, where a missed event is
  -- visible to the user. Powered-but-idle was still landing on 1 Hz -- which is the
  -- state a room sits in most of the day, and exactly what was loading the Director.
  local interval
  if gEventDriven          then interval = EVENT_HEARTBEAT_MS   -- events carry state
  elseif not gLastPower    then interval = IDLE_POLL_MS         -- off: nothing to track
  elseif not gLastPlaying  then interval = QUIET_POLL_MS        -- on, but nothing moving
  else                          interval = gPollMs end          -- playing, events quiet
  gPollTimer = C4:SetTimer(interval, function()
    if gConnected and gRoom then pcall(PushState, false) end
  end, true)
end

function StopPolling()
  -- L1: SetTimer returns a timer object (:Cancel) on current cores but a numeric
  -- handle on others — handle both so a stale timer can't keep firing.
  if gPollTimer then
    if type(gPollTimer) == "table" and gPollTimer.Cancel then pcall(function() gPollTimer:Cancel() end)
    else pcall(function() C4:KillTimer(gPollTimer) end) end
    gPollTimer = nil
  end
end

-- ============================================================================
-- Event-driven refresh (variable listeners)
-- ============================================================================
-- Watch the room's media vars + the digital-audio aggregator's queue/shuffle/repeat
-- vars so a state push happens the instant Control4 changes them, not up to one poll
-- interval later. The poll (Room Poll Interval) remains a safety net for any var that
-- updates silently — raise that interval once listeners are confirmed reliable.
local ROOM_WATCH_VARS = {
  ROOM_VAR.CURRENT_SELECTED_DEVICE, ROOM_VAR.CURRENT_AUDIO_DEVICE,
  ROOM_VAR.POWER_STATE, ROOM_VAR.CURRENT_VOLUME, ROOM_VAR.IS_MUTED,
  ROOM_VAR.CURRENT_MEDIA_INFO, ROOM_VAR.PLAYING_AUDIO_DEVICE,
}
local AGG_WATCH_VARS = { 1003, 1006 }   -- room_queue_settings, QUEUE_STATUS_V2

function WatchRoomVars()
  local rid = tonumber(gRoom); if not rid then return end
  for _, v in ipairs(ROOM_WATCH_VARS) do
    pcall(function() C4:RegisterVariableListener(rid, v) end)
  end
end

function UnwatchRoomVars()
  local rid = tonumber(gRoom); if not rid then return end
  for _, v in ipairs(ROOM_WATCH_VARS) do
    pcall(function() C4:UnregisterVariableListener(rid, v) end)
  end
end

-- Learned from room 1031 (<deviceid>); idempotent — no-op if already watching that id.
function WatchAggregator(aggId)
  aggId = tonumber(aggId)
  if not aggId or aggId == gAggWatchId then return end
  UnwatchAggregator()
  gAggWatchId = aggId
  for _, v in ipairs(AGG_WATCH_VARS) do
    pcall(function() C4:RegisterVariableListener(aggId, v) end)
  end
  dbg("watching aggregator", aggId)
end

function UnwatchAggregator()
  if not gAggWatchId then return end
  for _, v in ipairs(AGG_WATCH_VARS) do
    pcall(function() C4:UnregisterVariableListener(gAggWatchId, v) end)
  end
  gAggWatchId = nil
end

-- C4 callback: any watched variable changed. Two subscribers share this one entry
-- point now — the intercom's proxy level vars (ringer/speaker/mic sliders) and our
-- room/aggregator watches — so offer it to the intercom first and only do the room
-- refresh if it wasn't one of its variables.
function OnWatchedVariableChanged(idDevice, idVariable, strValue)
  -- Intercom level vars are watched by the separate intercom driver now; we only
  -- watch our own room/aggregator vars. Coalesce: streaming makes these vars tick
  -- rapidly, and a synchronous BuildState per tick pegged the Director thread.
  -- A change to our room's 1031 = we joined/left a session: refresh the early-out key.
  if idVariable == ROOM_VAR.CURRENT_MEDIA_INFO then updateSessionDev(strValue) end
  if gConnected and gRoom then SchedulePush() end
end

-- ============================================================================
-- Event-driven refresh (OS3+ MediaSession system events)
-- ============================================================================
-- Variable listeners above only cover vars this driver knows to watch; the
-- MediaSession system events are Director's own push channel for exactly the
-- now-playing/volume/mute state we render, and let us back the poll off to a
-- heartbeat. They are UNDOCUMENTED (public docs list ~13 of 109 events), so
-- EVERYTHING here is best-effort and guarded: if the events don't exist, don't
-- fire, or don't carry a room id, the poll (StartPolling) still gets us there.
--
-- Register/index STRICTLY BY NAME via the C4SystemEvents constants table: the
-- numeric slot ids shifted between Director versions (65/66/67, 101 moved), so a
-- literal number would silently subscribe to the wrong event on some controllers.
local MEDIA_SESSION_EVENTS = {
  "OnMediaSessionMediaInfoChanged",         -- now-playing metadata (title/artist/art/…)
  "OnMediaSessionVolumeLevelChanged",       -- room volume level
  "OnMediaSessionMuteStateChanged",         -- mute on/off
  "OnMediaSessionVolumeSliderStateChanged", -- volume-slider availability
}

function RegisterMediaSessionEvents()
  gRegisteredEvents = {}
  gMediaEventNames  = {}
  -- Pre-OS3 controllers have no C4SystemEvents table (or lack these names) -> we
  -- register nothing and the poll alone carries state. Silent degrade by design.
  if type(C4SystemEvents) ~= "table" then
    dbg("C4SystemEvents unavailable: MediaSession push off, polling only")
    return
  end
  for _, name in ipairs(MEDIA_SESSION_EVENTS) do
    local id = C4SystemEvents[name]
    if type(id) == "number" then
      -- arg 0 = subscribe globally (no per-device filter); we scope by room in the
      -- handler instead, since the payload — when it carries a room — is the only
      -- reliable discriminator on a controller serving many rooms.
      local ok = pcall(function() C4:RegisterSystemEvent(id, 0) end)
      if ok then
        gRegisteredEvents[#gRegisteredEvents + 1] = id
        gMediaEventNames[name] = true
      end
    end
    -- else: this OS doesn't expose this event name — skip it, the poll covers it.
  end
  dbg("MediaSession events registered:", #gRegisteredEvents, "of", #MEDIA_SESSION_EVENTS)
end

function UnregisterMediaSessionEvents()
  for _, id in ipairs(gRegisteredEvents) do
    pcall(function() C4:UnregisterSystemEvent(id, 0) end)
  end
  gRegisteredEvents = {}
  gMediaEventNames  = {}
end

-- Director callback for every subscribed system event. The payload is an
-- undocumented XML string; scrape the event name from it (the numeric id is not
-- passed the same way across cores) and only act on the MediaSession events we
-- registered — ignore anything else, per the forward-compat rule.
function OnSystemEvent(data)
  data = tostring(data or "")
  local name = data:match('name="(.-)"')
  if not name or not gMediaEventNames[name] then return end
  HandleMediaSessionEvent(data)
end

function HandleMediaSessionEvent(data)
  -- Opportunistic room-scoping. The payload MAY name a room; if it names one that
  -- is NOT ours, ignore it. When we can't tell (the usual, undocumented case) we
  -- refresh anyway — PushState dedupes, so a cross-room event costs one room-var
  -- read and zero wire traffic. Never skip on ambiguity, only on a positive miss.
  local evRoom = data:match('roomId="(%d+)"') or data:match('roomID="(%d+)"')
              or data:match('room="(%d+)"')   or data:match('roomid="(%d+)"')
  if evRoom and gRoom and evRoom ~= tostring(gRoom) then return end

  -- Session early-out (the N-instance fix). A MediaSessionMediaInfoChanged payload
  -- names its session device as <deviceid>. Every keypad instance receives every
  -- house-wide media event (global subscription); if we KNOW our room's session
  -- device and this event isn't it, the event is for a room we don't serve — drop
  -- it before arming any timer or building anything. Only skip on a positive miss:
  -- gRoomSessionDev nil (not learned) or the event carrying no deviceid (e.g. a
  -- volume event) falls through, so we never miss our own room's updates.
  local evDev = data:match("<deviceid>(%d+)</deviceid>")
  if evDev and gRoomSessionDev ~= nil and evDev ~= gRoomSessionDev then return end

  -- Coalesce straight through SchedulePush (250ms). The old 150ms re-arming debounce
  -- here could NEVER fire under continuous streaming (events < 150ms apart kept
  -- cancelling it), dropping live updates to the 10s heartbeat. SchedulePush's guard
  -- fires reliably on the trailing edge and merges with the var-change path.
  if gConnected and gRoom then SchedulePush() end

  -- Back the poll off to a heartbeat: events are carrying state now, so the 1s poll
  -- would just be redundant traffic. Only restart the timer on the transition.
  if not gEventDriven then
    gEventDriven = true
    StartPolling()
    dbg("MediaSession events active: poll -> heartbeat", EVENT_HEARTBEAT_MS, "ms")
  end

  -- Re-arm the quiet watchdog. If events stop (controller/OS where they don't fire
  -- reliably, or Director restart), resume fast polling so correctness never depends
  -- on the undocumented push continuing.
  if gEventQuiet then cancelTimer(gEventQuiet) end
  gEventQuiet = C4:SetTimer(EVENT_QUIET_MS, function()
    gEventQuiet = nil
    gEventDriven = false
    StartPolling()
    dbg("MediaSession events quiet: poll ->",
        (not gLastPower) and IDLE_POLL_MS or (gLastPlaying and gPollMs or QUIET_POLL_MS), "ms")
  end, false)
end

-- After a command, the source can take ~0.5-1s to actually act. Burst a few quick
-- re-reads so the UI reflects the change within ~150ms of the room updating,
-- instead of waiting up to a full poll interval. PushState dedupes, so only real
-- changes go out.
function PushStateSoon()
  for _, t in ipairs(gPushTimers) do cancelTimer(t) end   -- drop any still-pending burst
  gPushTimers = {}
  for _, ms in ipairs({ 120, 300, 550, 850, 1300 }) do
    gPushTimers[#gPushTimers + 1] =
      C4:SetTimer(ms, function() if gConnected and gRoom then pcall(PushState, false) end end, false)
  end
end

-- ============================================================================
-- Commands -> room
-- ============================================================================
local TRANSPORT_MAP = { play = "PLAY", pause = "PAUSE", stop = "STOP", next = "SKIP_FWD", prev = "SKIP_REV",
                        thumbsup = "THUMBS_UP", thumbsdown = "THUMBS_DOWN" }

function DoTransport(cmd)
  if not gRoom then return end
  -- The room ignores a PLAYPAUSE toggle here, so resolve play/pause to explicit
  -- PLAY/PAUSE from the CURRENT play state (read fresh; the status=OK_playing fix
  -- makes this accurate, so resume works again).
  if cmd == "playpause" then
    -- Use the last resolved play state rather than re-reading 1031.
    cmd = gLastPlaying and "pause" or "play"
  end
  -- Room power off: turn the whole room off (all its devices), matching the X4
  -- mini-player power button. Goes to the room proxy, not a source.
  if cmd == "roomoff" then
    dbg("ROOM_OFF -> room", gRoom)
    pcall(function() C4:SendToDevice(tonumber(gRoom), "ROOM_OFF", {}) end)
    PushStateSoon()
    return
  end
  -- Thumbs / shuffle / repeat go to the SOURCE device as its declared PROTOCOL command
  -- (ThumbUp/ThumbDown/ToggleShuffle/ToggleRepeat), not to the room — names vary per source.
  if cmd == "thumbsup" or cmd == "thumbsdown" or cmd == "shuffle" or cmd == "repeat" then
    local t
    if     cmd == "thumbsup"   then t = gThumbUp
    elseif cmd == "thumbsdown" then t = gThumbDown
    elseif cmd == "shuffle"    then t = gShuffle
    elseif cmd == "repeat"     then t = gRepeat end
    if t and t.dev and t.cmd then
      dbg("source cmd", t.cmd, "-> dev", t.dev)
      pcall(function() C4:SendToDevice(tonumber(t.dev), t.cmd, {}) end)
    end
    return
  end
  local c4 = TRANSPORT_MAP[cmd]
  if not c4 then dbg("unknown transport:", tostring(cmd)); return end
  dbg("transport", c4, "-> room", gRoom)
  C4:SendToDevice(tonumber(gRoom), c4, {})
  PushStateSoon()
end

function DoVolumeSet(level)
  if not gRoom or not level then return end
  level = math.max(0, math.min(100, math.floor(level)))
  dbg("SET_VOLUME_LEVEL", level, "-> room", gRoom)
  C4:SendToDevice(tonumber(gRoom), "SET_VOLUME_LEVEL", { LEVEL = level })
  PushStateSoon()
end

function DoVolumeStep(dir)
  if not gRoom then return end
  C4:SendToDevice(tonumber(gRoom), (dir == "up") and "VOL_UP" or "VOL_DOWN", {})
  PushStateSoon()
end

function DoMute()
  if not gRoom then return end
  C4:SendToDevice(tonumber(gRoom), "MUTE_TOGGLE", {})
  PushStateSoon()
end

function DoSelectSource(deviceId)
  if not gRoom or not deviceId then return end
  SelectSourceInRoom(gRoom, deviceId)
  gSources = nil          -- selection changed the room; re-enumerate next push
  PushStateSoon()
end

-- Select a source into a room via the verified room commands (control4-media-commands.md).
-- The streaming-vs-AV branch is MANDATORY: streaming sources (media_service — Apple Music /
-- SiriusXM / TuneIn / Pandora / AirPlay) IGNORE SELECT_AUDIO_DEVICE. They are selected
-- by sending DEVICE_SELECTED to the SOURCE with the target room; regular AV sources take
-- SELECT_AUDIO_DEVICE / SELECT_VIDEO_DEVICE on the ROOM (param key lowercase `deviceid`).
-- Branch on the source's `digital_audio_support` device-data flag (verified live,
-- room_control_keypad v139).
function SelectSourceInRoom(roomId, deviceId)
  roomId = tonumber(roomId); local idn = tonumber(deviceId)
  if not roomId or not idn then return end
  -- Streaming source? GetDeviceData(id,'digital_audio_support') == "true" -> media_service.
  local das = ""
  local okd, v = pcall(function() return C4:GetDeviceData(idn, "digital_audio_support") end)
  if okd and v ~= nil then das = tostring(v) end
  if string.lower(das) == "true" then
    -- Streaming source. The old code sent ONLY DEVICE_SELECTED to the source, on the
    -- assumption that streaming sources no-op on SELECT_AUDIO_DEVICE. That assumption
    -- is wrong: verified on a live media_service (id 2687) that DEVICE_SELECTED alone
    -- leaves the room OFF (POWER_STATE stays 0, nothing routed, favourites appeared to
    -- do nothing), while the room-level SELECT_AUDIO_DEVICE powers the room on AND
    -- routes it (POWER_STATE 0->1, CURRENT/PLAYING_AUDIO_DEVICE 0->2687).
    -- So: route via the ROOM first, then tell the source which room selected it.
    dbg("SELECT_AUDIO_DEVICE", idn, "-> room", roomId, "(streaming)")
    C4:SendToDevice(roomId, "SELECT_AUDIO_DEVICE", { deviceid = idn })
    dbg("DEVICE_SELECTED idRoom", roomId, "-> streaming source", idn)
    C4:SendToDevice(idn, "DEVICE_SELECTED", { idRoom = roomId })
    return
  end
  -- Regular AV source, sent to the room. M2: if the cache was just cleared, enumerate now
  -- so we resolve audio vs video correctly — otherwise everything defaults to
  -- SELECT_AUDIO_DEVICE and a video source gets routed as audio (and silently fails).
  if not gSources then EnumerateSources(roomId) end
  local kind = "audio"
  if gSources then
    for _, s in ipairs(gSources) do
      if s.id == tostring(deviceId) then kind = s.kind or "audio"; break end
    end
  end
  local cmd = (kind == "video") and "SELECT_VIDEO_DEVICE" or "SELECT_AUDIO_DEVICE"
  dbg(cmd, idn, "-> room", roomId)
  C4:SendToDevice(roomId, cmd, { deviceid = idn })
end

-- ============================================================================
-- Media write-path: favorites (broadcast-audio presets) + multiroom grouping
-- (Stage 4, Flavor 1 — all verified ROOM commands, no navigator identity).
-- ============================================================================
-- C4_DIGITAL_AUDIO — the Digital Media agent that owns multiroom sessions. It is the
-- session owner in room var 1031 (<deviceid>100002</deviceid>, verified var-1031 schema)
-- and the target of ADD_ROOMS_TO_SESSION. Prefer the id we actually learned (gAudioDev),
-- fall back to the well-known constant.
local DIGITAL_AUDIO_AGENT = 100002

local function digitalAudioAgent()
  local a = tonumber(gAudioDev)
  return (a and a > 0) and a or DIGITAL_AUDIO_AGENT
end

-- UI Configuration agent — owns the navigator "favorite tiles" users configure. It answers
-- GET_ALL_ROOM_FAVORITES_STATE synchronously to a plain SendUIRequest from any driver (no
-- navigator identity — verified live 2026-07-22).
local UI_CONFIG_AGENT = 1609


-- ── Exact media-service playback ("Play Item") ───────────────────────────────
-- A media-service favourite (Apple Music etc.) can be played EXACTLY -- not merely
-- "select the source and hope it resumes" -- via the service driver's `Play Item`
-- command. Verified live 2026-08-13 on Apple Music. See PROTOCOL.md "Favorites".
--
--   command: "Play Item"   (on the service's DRIVER device, not its media_service proxy)
--   params:  Item    = base64( JSON, below )
--            Room    = <roomId>
--            Shuffle = "Off" | "On"
--
-- The Item token is base64'd JSON. Composer stores a fat version (six artwork URLs and
-- an actions_list) but those are for ITS picker UI -- the driver only needs these, all
-- of which come straight out of the favourite we already parse:
--   { default_action="PlayStation", href=…, id=…, itemType=…, mediaKind="audio", title=… }
-- Passing the bare id or the raw href does NOT work; it must be this encoded object.
local B64C = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
local function b64encode(data)
  return ((data:gsub(".", function(x)
    local r, b = "", x:byte()
    for i = 8, 1, -1 do r = r .. (b % 2 ^ i - b % 2 ^ (i - 1) > 0 and "1" or "0") end
    return r
  end) .. "0000"):gsub("%d%d%d?%d?%d?%d?", function(x)
    if #x < 6 then return "" end
    local c = 0
    for i = 1, 6 do c = c + (x:sub(i, i) == "1" and 2 ^ (6 - i) or 0) end
    return B64C:sub(c + 1, c + 1)
  end) .. ({ "", "==", "=" })[#data % 3 + 1])
end

-- Fast base64 for the firmware relay. The b64encode above walks every BIT of every
-- byte and concatenates strings as it goes -- fine for a 200-byte Play Item token,
-- ruinous for the ~575 4 KB chunks of a firmware image on an EA-3's single Lua
-- thread. This does three bytes at a time through a lookup table into a buffer, and
-- prefers Control4's native encoder when the firmware exposes one.
local B64T = {}
for i = 1, 64 do B64T[i - 1] = B64C:sub(i, i) end
function b64chunk(data)
  if C4 and C4.Base64Encode then
    local ok, out = pcall(function() return C4:Base64Encode(data) end)
    if ok and type(out) == "string" and #out > 0 then return out end
  end
  -- ARITHMETIC only, no >> or & : DriverWorks is Lua 5.1 and the bitwise operators
  -- do not exist there. (A local luac 5.4 will happily accept them and the driver
  -- then fails to load on the Director -- checked the hard way.)
  local floor = math.floor
  local parts, n, i = {}, #data, 1
  while i + 2 <= n do
    local a, b, c = data:byte(i, i + 2)
    local v = a * 65536 + b * 256 + c
    parts[#parts + 1] = B64T[floor(v / 262144) % 64] .. B64T[floor(v / 4096) % 64] ..
                        B64T[floor(v / 64) % 64]     .. B64T[v % 64]
    i = i + 3
  end
  local rem = n - i + 1
  if rem == 1 then
    local v = data:byte(i) * 65536
    parts[#parts + 1] = B64T[floor(v / 262144) % 64] .. B64T[floor(v / 4096) % 64] .. "=="
  elseif rem == 2 then
    local a, b = data:byte(i, i + 1)
    local v = a * 65536 + b * 256
    parts[#parts + 1] = B64T[floor(v / 262144) % 64] .. B64T[floor(v / 4096) % 64] ..
                        B64T[floor(v / 64) % 64] .. "="
  end
  return table.concat(parts)
end

function urldecode(s)
  s = tostring(s or ""):gsub("+", " ")
  return (s:gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end))
end

-- JSON for the token. Hand-rolled (not json.encode) so key ORDER is stable and the
-- payload stays exactly the six fields the service accepts.
local function PlayItemToken(fav)
  if not (fav and fav.href and fav.itemId and fav.itemType) then return nil end
  local function esc(v) return tostring(v):gsub("\\", "\\\\"):gsub('"', '\\"') end
  local j = '{"default_action":"PlayStation"'
         .. ',"href":"'      .. esc(fav.href)     .. '"'
         .. ',"id":"'        .. esc(fav.itemId)   .. '"'
         .. ',"itemType":"'  .. esc(fav.itemType) .. '"'
         .. ',"mediaKind":"audio"'
         .. ',"title":"'     .. esc(fav.title or "") .. '"}'
  return b64encode(j)
end

-- Which device owns "Play Item"? NOT the media_service proxy the favourite names --
-- sending there is silently ignored (verified) -- but the service's DRIVER device.
--
-- The relationship is explicit in the project: the proxy carries an INPUT binding of
-- class MediaService (id 5001) whose PROVIDER is that driver device. Verified live:
--   item 2687 binding 5001 "Media Service" class=MediaService io=Input -> 2686
-- so ask Control4 for the bound provider rather than guessing at device numbering.
-- (An earlier version probed +/-3 ids around the source for one declaring the command.
-- That happened to work here because 2686/2687 are adjacent, but device ids are not
-- ordered by relationship and it would silently fall back to source-select elsewhere.)
local MEDIASERVICE_BINDING = 5001
local gPlayItemOwner = {}

local function declaresPlayItem(id)
  local d = id and CachedDeviceData(id)
  return (d and d:find("<name>Play Item</name>", 1, true)) and true or false
end

function PlayItemOwner(srcId)
  local s = tonumber(srcId); if not s then return nil end
  if gPlayItemOwner[s] ~= nil then return gPlayItemOwner[s] or nil end

  -- Preferred: the MediaService binding's provider.
  local owner
  local ok, v = pcall(function()
    return C4:GetBoundProviderDevice(s, MEDIASERVICE_BINDING)
  end)
  if ok and tonumber(v) and tonumber(v) > 0 then owner = tonumber(v) end
  if not owner then   -- some OS builds expose it as a table of {deviceid=…}
    local ok2, t = pcall(function() return C4:GetBoundProviderDevices(s) end)
    if ok2 and type(t) == "table" then
      for _, e in pairs(t) do
        local cand = tonumber(type(e) == "table" and (e.deviceid or e.id) or e)
        if cand and declaresPlayItem(cand) then owner = cand; break end
      end
    end
  end

  -- Trust the binding. declaresPlayItem() is only a HINT: C4:GetDeviceData on a
  -- service instance returns the generic proxy definition rather than the instance's
  -- own command list, so it does not see "Play Item" even on the correct device --
  -- verified live, where the binding correctly gave 2686 and this check then rejected
  -- it and fell back to source-select. The binding IS the relationship; if the command
  -- turns out not to exist there, SendToDevice is a harmless no-op.
  if owner then
    dbg("Play Item owner for source", s, "=", owner,
        "(MediaService binding" .. (declaresPlayItem(owner) and ", declares it" or "") .. ")")
    gPlayItemOwner[s] = owner
    return owner
  end
  dbg("no Play Item owner for source", s, "-- binding gave nothing; source select")
  gPlayItemOwner[s] = false
  return nil
end

-- Play a favorite the firmware tapped. Resolved from the gFavorites cache (built by
-- BuildFavoritesList) by favorite id. Two play paths (control4-media-commands.md):
--   * kind "broadcast" — an exact Broadcast-Audio media item: SELECT_AUDIO_MEDIA {mediaid}
--     into the room (fully verified, exact play).
--   * kind "stream" — a media-service (streaming) station/playlist tile: exact play is
--     navigator-GATED, so we fall back to selecting its SOURCE (DEVICE_SELECTED), which
--     resumes that source's content. Honest degradation, no gray dependency.
-- Legacy: a bare {mediaid} (no id) still plays broadcast-audio directly (Flavor-1 contract).
function DoPlayFavorite(msg)
  if not gRoom then return end
  -- Back-compat: Flavor-1 firmware sent {t=favorite, mediaid=..}.
  if type(msg) ~= "table" then msg = { mediaid = msg } end
  local fav = msg.id and gFavorites and gFavorites[tostring(msg.id)] or nil
  local mid = (fav and fav.mediaid) or msg.mediaid
  local kind = fav and fav.kind or (mid and "broadcast") or nil

  if kind == "broadcast" and mid and tostring(mid) ~= "" then
    dbg("favorite -> SELECT_AUDIO_MEDIA BROADCAST_AUDIO mediaid", mid, "room", gRoom)
    C4:SendToDevice(tonumber(gRoom), "SELECT_AUDIO_MEDIA",
      { deselect = "0", type = "BROADCAST_AUDIO", mediaid = tostring(mid) })
  elseif kind == "stream" and fav and tonumber(fav.src) then
    -- Preferred: play the EXACT item via the service driver's "Play Item".
    local tok   = PlayItemToken(fav)
    local owner = tok and PlayItemOwner(fav.src) or nil
    if tok and owner then
      dbg("favorite -> Play Item", tostring(fav.itemId), "on", owner, "room", gRoom)
      C4:SendToDevice(owner, "Play Item",
                      { Item = tok, Room = tostring(gRoom), Shuffle = "Off" })
    else
      -- Fallback: select the source and let it resume whatever it had. Used when the
      -- favourite carries no catalog context, or no device near the source declares
      -- "Play Item" (a service we have not seen).
      dbg("favorite (no Play Item path) -> select source", fav.src, "room", gRoom)
      SelectSourceInRoom(gRoom, fav.src)
    end
  else
    dbg("favorite: unresolved / unplayable id=", tostring(msg.id), "mediaid=", tostring(msg.mediaid))
    return
  end
  PushStateSoon()
end

-- Build the room's favorite tiles for the on-screen grid, from the UI Configuration agent
-- (navigator favorites — the real user-configured tiles, with artwork). Verified read; no
-- navigator identity. We filter to MEDIA favorites (streaming stations/playlists and any
-- broadcast-audio presets) and cache each by id in gFavorites so DoPlayFavorite can resolve
-- the play path. Category-launcher tiles (lights/shades/comfort/security/…) are dropped —
-- this is the media keypad's music-favorites grid.
--
-- Favorite XML (GET_ALL_ROOM_FAVORITES_STATE):
--   <favorite><id>..</id><path>..</path><title>..</title><image>..artURL..</image>
--            <menu>..</menu><type>..</type></favorite>
--   media path = /v1/rooms/{room}/mediaservicefavorite/{sourceDev}?context=..{catalogRef}..
function BuildFavoritesList()
  gFavorites = {}
  local rid = tonumber(gRoom); if not rid then return {} end
  local ok, xml = pcall(function()
    return C4:SendUIRequest(UI_CONFIG_AGENT, "GET_ALL_ROOM_FAVORITES_STATE", {})
  end)
  if not ok or not xml then dbg("getfavorites: agent 1609 read failed"); return {} end
  xml = tostring(xml)

  -- Narrow to our room's <favorites_state> block.
  local block
  for room in xml:gmatch("<room>(.-)</room>") do
    if tonumber(room:match("<room_id>(%d+)</room_id>") or "") == rid then
      block = room:match("<favorites_state>(.-)</favorites_state>"); break
    end
  end
  if not block then dbg("getfavorites: no favorites for room", rid); return {} end

  local list = {}
  for fav in block:gmatch("<favorite>(.-)</favorite>") do
    local id    = fav:match("<id>(.-)</id>") or ""
    local path  = fav:match("<path>(.-)</path>") or ""
    -- Unescape: these come straight out of the agent's XML, so an ampersand in a
    -- station name arrives as "&amp;" and was rendered literally on the panel.
    local title = unescape(fav:match("<title>(.-)</title>") or "")
    local image = fav:match("<image>(.-)</image>") or ""
    local entry
    -- Streaming (media-service) station/playlist favorite.
    local src = path:match("/mediaservicefavorite/(%d+)")
    if src then
      -- The query string carries the CATALOG ITEM this tile points at, flattened as
      --   context=href~/v1/catalog/us/stations/ra.123~id~ra.123~itemType~stations
      -- Those three fields are what "Play Item" needs to play this exact item (see
      -- PlayItemToken below), so keep them; without them all we can do is select the
      -- source and hope it resumes something.
      local ctx = path:match("[?&]context=([^&]*)") or ""
      ctx = urldecode(ctx)
      entry = { id = id, kind = "stream", src = tonumber(src),
                href = ctx:match("href~([^~]+)"),
                itemId = ctx:match("id~([^~]+)"),
                itemType = ctx:match("itemType~([^~]+)") }
    -- Broadcast-audio favorite (exact play by mediaid), if the tile carries one.
    elseif fav:match("<mediaid>(%d+)</mediaid>") then
      entry = { id = id, kind = "broadcast", mediaid = fav:match("<mediaid>(%d+)</mediaid>") }
    end
    if entry and id ~= "" then
      entry.title = (title ~= "") and title or "Favorite"
      gFavorites[id] = entry
      list[#list + 1] = { id = id, title = entry.title, image = image, kind = entry.kind }
    end
  end
  dbg("getfavorites: room", rid, "->", #list, "media favorites")
  return list
end

-- Owner room of the multiroom session our room is currently in (from var 1006,
-- QUEUE_STATUS_V2), or our own room id if we are not in a session yet (in which case a
-- join makes us the owner). Used to target ADD_ROOMS_TO_SESSION at the right session.
function RoomSessionOwner()
  local rid = tonumber(gRoom); if not rid then return nil end
  local okq, xml = pcall(function() return C4:GetDeviceVariable(digitalAudioAgent(), 1006) end)
  if okq and xml then
    for q in tostring(xml):gmatch("<queue>(.-)</queue>") do
      local block = q:match("<rooms>(.-)</rooms>") or ""
      for m in block:gmatch("<id>(%d+)</id>") do
        if tonumber(m) == rid then
          return tonumber(q:match("<owner>(%d+)</owner>")) or rid
        end
      end
    end
  end
  return rid   -- not in a session: our room becomes the owner
end

-- Multiroom grouping write side (the X4 add-rooms panel tap). JOIN adds a room to our
-- session via the verified ADD_ROOMS_TO_SESSION on the Digital Media agent. LEAVE has no
-- verified counterpart command (the reference documents only the ADD side), so we use the
-- verified ROOM_OFF on that room — turning an audio room off removes it from the session.
-- Conservative choice, flagged: revisit if a verified REMOVE_ROOMS_FROM_SESSION surfaces.
function DoGroupRoom(id, join)
  local rid = tonumber(id)
  if not gRoom or not rid then return end
  local doJoin = (join == true) or (tostring(join) == "true") or (tostring(join) == "1")
  if doJoin then
    local owner = RoomSessionOwner() or tonumber(gRoom)
    dbg("ADD_ROOMS_TO_SESSION owner", owner, "add room", rid)
    pcall(function()
      C4:SendToDevice(digitalAudioAgent(), "ADD_ROOMS_TO_SESSION",
        { ROOM_ID = owner, ROOM_ID_LIST = tostring(rid) })
    end)
  else
    dbg("leave session -> ROOM_OFF room", rid)
    pcall(function() C4:SendToDevice(rid, "ROOM_OFF", {}) end)
  end
  PushStateSoon()
end

-- ============================================================================
-- Helpers
-- ============================================================================
function DeviceName(deviceId)
  local id = tonumber(deviceId)
  if not id or id == 0 then return "" end   -- id 0 resolves to this driver's name
  local ok, name = pcall(function() return C4:GetDeviceDisplayName(id) end)
  if ok and type(name) == "string" then return name end
  return ""
end

-- GetDeviceData is heavy and a source's data is static, so cache it per device.
-- Both the icon picker and the transport-capability parser read from this.
local gDevDataCache = {}
function CachedDeviceData(deviceId)
  local id = tonumber(deviceId)
  if not id or id == 0 then return nil end
  if gDevDataCache[id] == nil then
    local ok, data = pcall(function() return C4:GetDeviceData(id) end)
    gDevDataCache[id] = (ok and type(data) == "string") and data or false
  end
  return gDevDataCache[id] or nil
end

-- Layer 1: a source declares which transport buttons it supports in its
-- <Dashboard><Transport><ButtonType>… (verified live: SiriusXM = PLAY+STOP only,
-- Pandora = PAUSE+SKIP_FWD+THUMBS_UP/DOWN). Parse them into capability flags so the
-- device renders Stop-vs-play/pause, hides skip, and shows thumbs to match Navigator.
-- Parsed-caps memo. The <Dashboard> is static per source (comment below), and its
-- raw XML is already cached in gDevDataCache — but re-running this gmatch loop on
-- every BuildState was pure waste under a streaming storm. Cache the PARSED result
-- keyed by source id; cleared on reload with everything else (session-lifetime,
-- same staleness contract as gDevDataCache).
local gCapsCache = {}
function GetSourceCaps(deviceId)
  local cid = tonumber(deviceId)
  if cid and gCapsCache[cid] then return gCapsCache[cid] end
  local caps = { found = false, pause = false, stop = false, nextt = false,
                 prev = false, thumbsUp = false, thumbsDown = false,
                 thumbUpCmd = nil, thumbDownCmd = nil,
                 shuffle = false, repeatt = false, shuffleCmd = nil, repeatCmd = nil }
  local data = CachedDeviceData(deviceId)
  if not data then return caps end
  local dash = data:match("<Dashboard>(.-)</Dashboard>")
  if not dash then return caps end
  caps.found = true
  -- Standard buttons key off <ButtonType>. CUSTOM buttons (thumbs/shuffle/repeat) key
  -- off <IconId>, NOT <Id>: the icon name is stable across services, but <Id> is not
  -- (verified live: Apple declares thumbs as "ThumbsUp"/"ThumbsDown", Pandora as
  -- "ThumbUp"/"ThumbDown" — matching <Id> only ever worked for one service). The icon
  -- names are identical everywhere: amt_thumbsup / amt_thumbsdn / amt_shuffle / amt_repeat
  -- (the *_on variants are the "cancel/off" toggles). SiriusXM = PLAY+STOP only.
  -- NB: Lua %w excludes "_", so SKIP_FWD/SKIP_REV need [%w_].
  for tr in dash:gmatch("<Transport>(.-)</Transport>") do
    local bt = tr:match("<ButtonType>([%w_]+)</ButtonType>")
    local icon = (tr:match("<IconId>(.-)</IconId>") or ""):gsub("%s+", ""):lower()
    local relCmd = tr:match("<ReleaseCommand>.-<Name>(.-)</Name>")
    if     bt == "PAUSE"    then caps.pause = true
    elseif bt == "STOP"     then caps.stop  = true
    elseif bt == "SKIP_FWD" then caps.nextt = true
    elseif bt == "SKIP_REV" then caps.prev  = true
    elseif bt == "CUSTOM" then
      -- amt_thumbsup / amt_thumbsdn = the primary rate action (the *_on variants are the
      -- "cancel like/dislike" toggles — ignore those; we model thumbs as momentary).
      if     icon == "amt_thumbsup" then caps.thumbsUp   = true; caps.thumbUpCmd   = relCmd
      elseif icon == "amt_thumbsdn" then caps.thumbsDown = true; caps.thumbDownCmd = relCmd
      -- Shuffle/Repeat list BOTH On+Off transports (amt_shuffle + amt_shuffle_on), both
      -- mapping to the same toggle command — capturing either is enough.
      elseif icon:match("^amt_shuffle") then caps.shuffle = true; caps.shuffleCmd = caps.shuffleCmd or relCmd
      elseif icon:match("^amt_repeat")  then caps.repeatt = true; caps.repeatCmd  = caps.repeatCmd  or relCmd
      end
    end
  end
  -- caps = the source's declared <Dashboard> for the current media (static per source;
  -- resolved live via medSrcDev). This is the authoritative readable control list — the
  -- only per-track masking Navigator adds lives in the unreadable DashboardChanged event.
  if cid then gCapsCache[cid] = caps end
  return caps
end

local gIconCache = {}
function DeviceIcon(deviceId)
  local id = tonumber(deviceId)
  if not id or id == 0 then return "" end
  if gIconCache[id] ~= nil then return gIconCache[id] end
  local data = CachedDeviceData(id)
  if not data then return "" end
  -- M4: a service exposes its tile icon at several sizes
  -- (.../icons/device/experience_70|90|300|512|1024.png) plus dozens of tiny
  -- transport-control icons. Pick the largest "device" icon <= 512 (fills the
  -- screen, fast to fetch) — the old "first png" grabbed a 70px control icon.
  local sizes = {}
  for u, s in data:gmatch("(controller://[^\"'<>%s]+/device/[^\"'<>%s]-_(%d+)%.png)") do
    sizes[tonumber(s)] = u
  end
  local url, bestSz
  for sz, u in pairs(sizes) do
    if sz <= 512 and (not bestSz or sz > bestSz) then url, bestSz = u, sz end
  end
  if not url then for sz, u in pairs(sizes) do if not bestSz or sz < bestSz then url, bestSz = u, sz end end end
  -- fallbacks: any /device/ icon, then any controller png
  url = url or data:match("(controller://[^\"'<>%s]+/device/[^\"'<>%s]+%.png)")
            or data:match("(controller://[^\"'<>%s]+%.png)")
  if not url then gIconCache[id] = ""; return "" end
  -- Resolve controller:// to the master controller's web server so the device
  -- can fetch it directly (no dependency on a configured host).
  local ok2, ctrl = pcall(function() return C4:GetControllerNetworkAddress() end)
  if ok2 and ctrl and tostring(ctrl) ~= "" then
    url = url:gsub("^controller://", "http://" .. tostring(ctrl) .. "/")
  end
  -- Static per source (icon URL + controller address don't change mid-session);
  -- memo so the per-BuildState gmatch scan doesn't repeat under a streaming storm.
  gIconCache[id] = url
  return url
end

-- Scan a device's variables for now-playing data, adaptively. Returns
-- (mediaInfoXml, currentAppName) — either may be nil. Works for any source whose
-- integration exposes media as a <mediainfo> blob and/or a "current app" var.
function ScanDeviceMedia(deviceId)
  local id = tonumber(deviceId)
  if not id or id == 0 then return nil, nil end
  local ok, t = pcall(function() return C4:GetDeviceVariables(id) end)
  if not ok or type(t) ~= "table" then return nil, nil end
  local mediaXml, app
  for _, info in pairs(t) do
    if type(info) == "table" then
      local name = tostring(info.name or ""):upper()
      local val  = tostring(info.value or "")
      if not mediaXml and val:find("<mediainfo") then mediaXml = val end
      if not app and (name:find("CURRENT.-APP") or name:find("CURRENT_APP")
                      or name == "APP" or name:find("APP.-NAME")) and #val > 0 then
        app = val
      end
    end
  end
  return mediaXml, app
end

-- Selectable sources for a room — the Navigator's Listen/Watch lists, via the room
-- commands GET_LISTEN_DEVICES / GET_WATCH_DEVICES (each returns
-- <sources><source><id/><type/></source>…). We keep real source types and resolve
-- names with GetDeviceDisplayName.
-- Cached per connection (reset on connect / on source change).
local SOURCE_KIND = {   -- type -> which room SELECT_* command applies
  DIGITAL_AUDIO_SERVER = "audio", AUDIO_SELECTION = "audio",
  VIDEO_SELECTION = "video", VIDEO_DEVICE = "video", DEVICE = "video",
}

function EnumerateSources(roomId)
  if gSources and #gSources > 0 then return gSources end   -- cache only non-empty
  -- Both lists, always: the filter behind this was a "Sources" property for an
  -- on-screen source picker that does not exist. The list is still needed to tell
  -- audio sources from video ones when selecting one for a favourite.
  local cmds = { "GET_LISTEN_DEVICES", "GET_WATCH_DEVICES" }
  local out, seen = {}, {}
  for _, cmd in ipairs(cmds) do
    local ok, xml = pcall(function() return C4:SendToDevice(tonumber(roomId), cmd, {}) end)
    if ok and type(xml) == "string" then
      for id, typ in xml:gmatch("<source><id>(%d+)</id><type>(.-)</type></source>") do
        local kind = SOURCE_KIND[typ]
        if kind and not seen[id] then
          local name = DeviceName(id)     -- "" for SPECIAL_AUDIO / unnamed -> dropped
          if name ~= "" then
            seen[id] = true
            out[#out + 1] = { id = id, name = Normalize(name), kind = kind }
          end
        end
      end
    else
      dbg("EnumerateSources:", cmd, "error:", tostring(xml))
    end
  end
  -- keep Navigator order (Listen then Watch); don't alpha-sort
  dbg("EnumerateSources:", #out, "sources for room", tostring(roomId))
  if #out > 0 then gSources = out end
  return out
end

-- The LVGL fonts on the device only cover basic ASCII, so transliterate common
-- UTF-8 punctuation + Latin-1 accents to ASCII and strip the rest. (For full
-- fidelity, build an extended font on the firmware instead.)
local UTF8_MAP = {
  ["\226\128\152"] = "'", ["\226\128\153"] = "'",     -- ' '
  ["\226\128\156"] = '"', ["\226\128\157"] = '"',     -- " "
  ["\226\128\147"] = "-", ["\226\128\148"] = "-",     -- – —
  ["\226\128\166"] = "...", ["\226\128\162"] = "*",   -- … •
  ["\195\160"]="a",["\195\161"]="a",["\195\162"]="a",["\195\163"]="a",["\195\164"]="a",["\195\165"]="a",
  ["\195\167"]="c",["\195\168"]="e",["\195\169"]="e",["\195\170"]="e",["\195\171"]="e",
  ["\195\172"]="i",["\195\173"]="i",["\195\174"]="i",["\195\175"]="i",
  ["\195\177"]="n",["\195\178"]="o",["\195\179"]="o",["\195\180"]="o",["\195\181"]="o",["\195\182"]="o",
  ["\195\184"]="o",["\195\185"]="u",["\195\186"]="u",["\195\187"]="u",["\195\188"]="u",["\195\189"]="y",["\195\191"]="y",
  ["\195\128"]="A",["\195\129"]="A",["\195\130"]="A",["\195\131"]="A",["\195\132"]="A",["\195\133"]="A",
  ["\195\135"]="C",["\195\136"]="E",["\195\137"]="E",["\195\138"]="E",["\195\139"]="E",
  ["\195\145"]="N",["\195\146"]="O",["\195\147"]="O",["\195\148"]="O",["\195\149"]="O",["\195\150"]="O",["\195\152"]="O",
  ["\195\153"]="U",["\195\154"]="U",["\195\155"]="U",["\195\156"]="U",["\195\159"]="ss",
}

-- The firmware now uses an extended LVGL font (Latin-1 + punctuation), so send
-- raw UTF-8 and let it render. (UTF8_MAP kept as a reference / future fallback.)
function Normalize(s)
  return s or ""
end

-- Global, and defined here only for locality with Normalize(). It is used from
-- BuildFavoritesList() far EARLIER in this file: as a `local` it was not in
-- lexical scope there, so favourite titles went out raw and the panel rendered
-- "Taylor Swift &amp; Similar Artists Station".
function unescape(s)
  if not s then return "" end
  s = s:gsub("&amp;", "&"):gsub("&lt;", "<"):gsub("&gt;", ">")
       :gsub("&quot;", '"'):gsub("&apos;", "'")
  return Normalize(s)
end

-- H2/H3: 1-entry memo. The art <img> blob is identical across polls until the
-- track changes, so caching the last decode avoids re-running this O(n) bit-twiddle
-- (and the whole expensive parse) every poll / every PushStateSoon burst shot.
local gB64In, gB64Out = nil, nil
local function b64decode(data)
  if not data or data == "" then return "" end
  if data == gB64In then return gB64Out end
  local key = data
  local b = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/'
  data = tostring(data):gsub('[^' .. b .. '=]', '')
  local out = (data:gsub('.', function(x)
    if x == '=' then return '' end
    local r, f = '', (b:find(x) - 1)
    for i = 6, 1, -1 do r = r .. (f % 2 ^ i - f % 2 ^ (i - 1) > 0 and '1' or '0') end
    return r
  end):gsub('%d%d%d?%d?%d?%d?%d?%d?', function(x)
    if #x ~= 8 then return '' end
    local c = 0
    for i = 1, 8 do c = c + (x:sub(i, i) == '1' and 2 ^ (8 - i) or 0) end
    return string.char(c)
  end))
  gB64In, gB64Out = key, out
  return out
end

-- Parse CURRENT_MEDIA_INFO (var 1031). Verified schema on this Director.
-- <meta> fields → ordered label/value pairs for the track-info ("i") panel.
local META_ORDER  = { "audioFormat", "sampleRate", "bitDepth", "bitRate", "channels", "audioQuality" }
local META_LABELS = {
  audioFormat = "Format", sampleRate = "Sample Rate", bitDepth = "Bit Depth",
  bitRate = "Bit Rate", channels = "Channels", audioQuality = "Quality",
}

function ParseMediaInfo(raw)
  local out = { playing = false, title = "", artist = "", album = "", mediaTypeV2 = "",
                artUrl = "", duration = 0, position = 0, srcDev = "", meta = {} }
  if not raw or raw == "" then return out end
  raw = tostring(raw)
  local function tag(name) return raw:match("<" .. name .. ">(.-)</" .. name .. ">") end
  out.title  = unescape(tag("title")  or "")
  out.artist = unescape(tag("artist") or "")
  out.album  = unescape(tag("album")  or "")
  out.srcDev = tag("medSrcDev") or ""
  -- mediatypeV2 (GENERIC_MEDIA, SONG, …) is the newer, richer classifier; fall back
  -- to the legacy <mediatype>. Forwarded so the UI can adapt (e.g. radio vs track).
  out.mediaTypeV2 = (tag("mediatypeV2") or tag("mediatype") or "")
  local img = tag("img")
  if img and img ~= "" then out.artUrl = b64decode(img) end
  local ss = tag("streamStatus") or ""
  -- Real transport state is the trailing status=OK_xxx; match OK_ explicitly so we
  -- don't grab "drmstatus=<base64>" earlier in the string.
  out.playing = (ss:match("status=(OK_%a+)") == "OK_playing")
  local dur = ss:match("duration=(%d+)")
  if dur then out.duration = math.floor(tonumber(dur) / 1e9) end
  -- Best-effort current position so the device can SEED its progress bar (it otherwise
  -- runs a purely local clock from 0 each track — wrong after a seek or when the
  -- keypad joins mid-track). Field name varies by source; try the common ones, same
  -- nanosecond units as duration. Absent -> stays 0 and the device keeps its local clock.
  local pos = ss:match("position=(%d+)") or ss:match("elapsed=(%d+)") or ss:match("offset=(%d+)")
  if pos then out.position = math.floor(tonumber(pos) / 1e9) end
  -- Radio/station detection from the CURRENT content (not the source's static Dashboard):
  -- a station carries a <stationid>, and the stream reports audioquality=Radio. Radio has
  -- no navigable queue, so skip-back / shuffle / repeat / thumbs don't apply (Navigator
  -- hides them; e.g. Apple Music radio = play/pause + skip-forward only).
  -- NOTE: deliberately NO isRadio flag any more. It was derived from <stationid> and
  -- audioquality=Radio, and both are wrong: Apple reports audioquality=Radio as a
  -- BITRATE TIER on ordinary album tracks, and carries a <stationid> on navigable
  -- content too. What is navigable now comes from the live queue -- see QueueLive().
  -- Track-info panel: pull <meta> as ordered {label,value} pairs.
  local meta = raw:match("<meta>(.-)</meta>")
  if meta then
    for _, k in ipairs(META_ORDER) do
      local v = meta:match("<" .. k .. ">(.-)</" .. k .. ">")
      if v and v ~= "" then
        if k == "bitRate" and tonumber(v) then v = math.floor(tonumber(v) / 1000) .. " kbps" end
        out.meta[#out.meta + 1] = { label = META_LABELS[k], value = unescape(v) }
      end
    end
  end
  return out
end
