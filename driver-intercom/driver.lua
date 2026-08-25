--[[ ===========================================================================
  MMKeypad Intercom — Control4 DriverWorks driver (companion to the keypad driver)
  ---------------------------------------------------------------------------
  Hosts the Control4 intercom proxy (intercomproxy, binding 5001). The intercom
  CANNOT live in the keypad driver (that's a <combo>; intercomproxy can't be in a
  combo driver), so this is a separate non-combo driver.

  It reaches the device only through the keypad driver: the keypad owns the device's
  :6700 link and RELAYS our SIP/call traffic via C4:SendToDevice over a control
  binding (class MMKEYPAD_INTERCOM). No network binding here.

  Relay wire (driver<->driver, via ExecuteCommand):
    keypad -> us : MMK_HELLO {from}      (the keypad's device id)
                   MMK_RX {json}         (a device line: sipstate/callstate)
    us -> keypad : MMK_WHOIS {}          (ask the keypad to (re)announce its id)
                   MMK_TX {json}         (a device line to write to :6700: sip/call)

  The intercom proxy contract lives in intercom_proxy/*.lua (bundled from Control4's
  reference Universal SIP Phone + the current intercom proxy-protocol docs).
=========================================================================== --]]

DRIVER_VERSION = "dev"   -- placeholder; build.sh stamps the real version (version.txt) into the .c4z
INTERCOM_PROXY = 5001     -- our intercomproxy binding
RELAY_BINDING  = 700      -- control binding to the keypad driver
EVENT_DOOR_1   = 1        -- "Door Action 1" pressed on a panel's call screen
EVENT_DOOR_2   = 2        -- "Door Action 2"
SIP_TRANSPORT  = "tcp"    -- TCP REQUIRED: device is on a different subnet from Director;
                          -- inbound UDP INVITEs are firewalled, but a persistent TCP
                          -- connection carries them back (like the UniFi/native stations).

local gKeypad   = nil     -- device id of the MMKeypad Media Keypad (relay peer)
local gSipReg   = false   -- device-reported SIP registration
local gSession  = 0       -- synthesized intercom call session id
local gInCall   = false   -- a SIP call is active (CS_BUSY)
local gTx       = 0       -- diagnostic: MMK_TX messages sent to the keypad
local gRx       = 0       -- diagnostic: MMK_RX messages received from the keypad
-- Native proxy call-behavior settings, polled from our own GET_DEVICE props (the intercom
-- proxy config panel is the single source of truth — no driver properties for these).
-- nil until the first poll.
local gProxyAuto          -- Auto Answer   (bool)
local gProxyMon           -- Monitor Mode  (bool)
local gProxyChime         -- Play Door Chime (bool)

-- JSON (bundled lib, else C4 built-in fallback).
local json
do
  local ok, mod = pcall(require, "json")
  if ok and type(mod) == "table" and mod.encode then json = mod
  else json = { encode = function(v) return C4:JsonEncode(v, false, true) end,
                decode = function(s) return C4:JsonDecode(s) end } end
end

-- Bundled Control4 intercom proxy contract (constants/notify/command/protocol/debug).
-- PROTOCOL.* -> INTERCOM.send_to_intercom -> MMK_SendDevice (relay); NOTIFY.* -> proxy.
require "intercom_proxy.intercom_debug"
require "intercom_proxy.intercom_constants"
require "intercom_proxy.intercom_notify"
require "intercom_proxy.intercom_protocol"
require "intercom_proxy.intercom_command"

-- ── logging / status ────────────────────────────────────────────────────────
-- tostring() every argument and cap the line: table.concat() throws on a boolean or
-- table and mangles a nil in the middle, and some of what is logged here is
-- device-supplied. Logging must not be able to break its caller, or flood the
-- Director's error log. (Same treatment as the keypad and agent drivers.)
local DBG_MAX = 400
local function dbg(...)
  local mode = Properties and Properties["Debug Logging"] or "Print"
  if mode == "Off" then return end
  local parts = {}
  for i = 1, select("#", ...) do parts[#parts + 1] = tostring((select(i, ...))) end
  local msg = "MMKpIntercom: " .. table.concat(parts, " ")
  if #msg > DBG_MAX then msg = msg:sub(1, DBG_MAX) .. "…(+" .. (#msg - DBG_MAX) .. ")" end
  if mode == "Print" or mode == "Print and Log" then print(msg) end
  if (mode == "Log" or mode == "Print and Log") and C4.ErrorLog then C4:ErrorLog(msg) end
end
local function setRelay(s) pcall(function() C4:UpdateProperty("Relay", s) end) end
-- Diagnostic relay status string (shown in the read-only "Relay" property).
local function relayStatus()
  if not gKeypad then return "Not linked" end
  return string.format("Linked  kp=%s  tx=%d  rx=%d", tostring(gKeypad), gTx, gRx)
end
local function syncIntercomDebug()
  local mode = Properties and Properties["Debug Logging"] or "Print"
  INTERCOM_DEBUG.PRINT = (mode == "Print" or mode == "Print and Log")
  INTERCOM_DEBUG.LOG   = (mode == "Log"   or mode == "Print and Log")
end
local function selfDeviceId()
  local ok, id = pcall(function() return C4:GetDeviceID() end)
  return (ok and id) or 0
end

-- ── relay seam: the bundled PROTOCOL.* modules push device control through here ──
-- Send a control object (Lua table) to the device, via the keypad relay.
function MMK_SendDevice(obj)
  if not gKeypad then dbg("no keypad link yet; dropping", tostring(obj and obj.t)); return end
  local ok, enc = pcall(json.encode, obj)
  if not ok then dbg("encode failed"); return end
  pcall(function() C4:SendToDevice(gKeypad, "MMK_TX", { json = enc, from = selfDeviceId() }) end)
  gTx = gTx + 1; setRelay(relayStatus())
end

-- ── lifecycle ───────────────────────────────────────────────────────────────
function OnDriverInit(initType)
  C4:UpdateProperty("Driver Version", DRIVER_VERSION)
  setRelay("Not linked")
  -- Read-only identity var so the NuVoxel Agent can reconcile this intercom to its
  -- keypad on resync (our room == the keypad's room) and never install a duplicate —
  -- the intercom analogue of the keypad's NV_HWID. Set in LateInit once the room is known.
  pcall(function() C4:AddVariable("NV_ROOM", "", "STRING", true, true) end)
end

function OnDriverLateInit(initType)
  syncIntercomDebug()
  pcall(function() C4:SetVariable("NV_ROOM", tostring(C4:RoomGetId() or "")) end)  -- identity for agent reconcile
  INTERCOM_STATE.monitorMode = MonitorOn()   -- seed runtime state from the property
  BindProxyLevelVars()                       -- map the proxy's RINGER/SPEAKER/MIC var ids
  StartLevelPoll()                           -- poll them -> forward slider changes to device
  gSipReg = false; gInCall = false
  pcall(function() NOTIFY.Current_State_Changed(CURRENT_STATE.CS_NOTREADY) end)
  -- Rediscover the keypad from the relay binding. OnBindingChanged does NOT re-fire for
  -- an already-bound binding on driver reload / Director restart, so without this the
  -- endpoint would sit unprovisioned (gKeypad nil -> "no keypad link yet") until the
  -- device next reboots. Ask the keypad to (re)announce; its MMK_HELLO sets gKeypad and
  -- provisions SIP. Purely additive — if the API/binding isn't ready, the device-hello
  -- path still recovers it.
  pcall(function()
    local kp = C4:GetBoundProviderDevice(0, RELAY_BINDING)
    kp = tonumber(kp)
    if kp and kp > 0 then
      gKeypad = kp
      setRelay(relayStatus())
      C4:SendToDevice(gKeypad, "MMK_WHOIS", { from = selfDeviceId() })
      PushSipConfig()
    end
  end)
end

function OnDriverDestroyed(initType)
  StopLevelPoll()
  gKeypad = nil; gSipReg = false; gInCall = false
  INTERCOM_DEBUG.END()
end

function OnPropertyChanged(prop)
  dbg("property changed:", prop)
  if prop == "Debug Logging" then syncIntercomDebug() end
  -- Auto Answer / Monitor Mode / Play Door Chime are NOT driver properties — they follow
  -- the native intercom proxy settings (see PollDeviceState).
end

-- ── relay binding to the keypad driver ──────────────────────────────────────
function OnBindingChanged(idBinding, strClass, bIsBound, otherDeviceID, otherBindingID)
  if idBinding ~= RELAY_BINDING then return end
  dbg("relay binding", tostring(bIsBound), "peer", tostring(otherDeviceID))
  if bIsBound then
    gKeypad = tonumber(otherDeviceID) or gKeypad
    setRelay(relayStatus())
    -- Ask the keypad to (re)announce its authoritative Lua device id (the binding's
    -- otherDeviceID is usually correct, but MMK_HELLO confirms it), then provision SIP.
    pcall(function() if gKeypad then C4:SendToDevice(gKeypad, "MMK_WHOIS", { from = selfDeviceId() }) end end)
    PushSipConfig()
  else
    gKeypad = nil; gSipReg = false
    setRelay("Not linked")
    pcall(function() NOTIFY.Current_State_Changed(CURRENT_STATE.CS_NOTREADY) end)
  end
end

-- ── inbound relay commands (driver<->driver via C4:SendToDevice) ─────────────
function ExecuteCommand(cmd, params)
  params = params or {}
  if cmd == "MMK_HELLO" then
    gKeypad = tonumber(params.from) or gKeypad
    setRelay(relayStatus())
    dbg("keypad link =", tostring(gKeypad))
    PushSipConfig()
  elseif cmd == "MMK_RX" then
    gRx = gRx + 1; setRelay(relayStatus())
    if params.json then HandleDeviceLine(tostring(params.json)) end
  elseif cmd == "ReRegister" then
    PushSipConfig()
  elseif cmd == "LUA_ACTION" and params.ACTION == "ReRegister" then
    PushSipConfig()
  end
end

-- ── device (relayed) -> intercom proxy ──────────────────────────────────────
function HandleDeviceLine(line)
  local ok, msg = pcall(json.decode, line)
  if not ok or type(msg) ~= "table" then return end
  if msg.t == "sipstate" then HandleSipState(msg)
  elseif msg.t == "callstate" then HandleCallState(msg)
  elseif msg.t == "callctl" then HandleCallCtl(msg) end
  -- unknown types ignored (forward-compat)
end

-- On-screen call controls coming back from the panel.
--   mute  -> mirror to the proxy so Navigator's own call UI agrees with the panel
--            (MUTE_CALL is the driver->device direction; this is the report back).
--   door  -> the door's action buttons. Control4's intercom proxy has NO door/relay
--            command and GET_DEVICE_LIST's device_props carry no relay info
--            (hasCamera/hasDisplay/hasButtons/isDoorStation only), so there is
--            nothing to discover. The dealer labels the buttons here and wires the
--            matching EVENT in programming to whatever the door actually uses --
--            a relay, a lock, a macro. LAST_DOOR_PEER carries which door it was,
--            so one event can serve several door stations.
function HandleCallCtl(msg)
  local a = tostring(msg.action or "")
  if a == "mute" then
    local on = (msg.on == true)
    dbg("callctl: mute", tostring(on))
    pcall(function() NOTIFY.Mute_Audio_Changed(on) end)
  elseif a == "door" then
    local id   = tostring(msg.id or "")
    local peer = tostring(msg.remote or "")
    local ev   = (id == "2") and EVENT_DOOR_2 or EVENT_DOOR_1
    dbg("callctl: door action", id, "peer", peer)
    pcall(function() C4:SetVariable("LAST_DOOR_PEER", peer) end)
    pcall(function() C4:FireEvent(ev) end)
  end
end

function HandleSipState(msg)
  gSipReg = (msg.registered == true)
  pcall(function() C4:UpdateProperty("SIP Registration", gSipReg and "Registered" or "Unregistered") end)
  if msg.user and tostring(msg.user) ~= "" then NOTIFY.Sip_Username_Changed(tostring(msg.user)) end
  NOTIFY.Current_State_Changed(gSipReg and CURRENT_STATE.CS_IDLE or CURRENT_STATE.CS_NOTREADY)
  -- Proactively assert the FULL device-state snapshot once registered. The proxy
  -- withholds SET_* (volume/DND/monitor) commands until the endpoint reports itself
  -- controllable; we only sent CURRENT_STATE_CHANGED before (and DEVICE_STATE_CHANGED
  -- only in reply to GET_STATE, which never arrives), so the proxy may have judged us
  -- NotReady-to-command. Pushing the full state on register marks us controllable.
  if gSipReg then pcall(SendIntercomDeviceState) end
  dbg("SIP", gSipReg and "registered" or "unregistered")
end

-- audio-only station: audio=true, video=false; session synthesized if absent.
-- remoteAor carried as remoteDeviceId for now (LIVE-VALIDATE the exact id the proxy
-- expects on a Director).
function HandleCallState(msg)
  local ev = tostring(msg.event or "")
  local dev = selfDeviceId()
  -- We deliberately do NOT report Incoming_Call (that made the proxy try to manage
  -- the call and auto-decline it). But we DO report Accepted/Ended once a call is up
  -- so the Communications agent tracks the live call and tears it down promptly
  -- instead of waiting on a media-timeout (the ~25s hang the app shows). The Director
  -- still owns ring/answer via the SIP dialog; this just keeps the agent in sync.
  if ev == "outgoing" then
    -- The keypad placed a call from its Intercom picker. Report OUTGOING_CALL so the
    -- Communications agent tracks it (remote id unresolved here — best-effort).
    gInCall = true
    gSession = msg.session or (gSession + 1)
    pcall(function() NOTIFY.Current_State_Changed(CURRENT_STATE.CS_BUSY) end)
    pcall(function() NOTIFY.Outgoing_Call(dev, gSession, 0, 0, true, false) end)
  elseif ev == "accepted" then
    gInCall = true
    gSession = msg.session or (gSession + 1)
    pcall(function() NOTIFY.Current_State_Changed(CURRENT_STATE.CS_BUSY) end)
    pcall(function() NOTIFY.Call_Accepted(dev, gSession, true, false) end)
  elseif ev == "ended" or ev == "rejected" then
    gInCall = false
    pcall(function() NOTIFY.Call_Ended(dev, gSession, "remote") end)
    pcall(function() NOTIFY.Current_State_Changed(gSipReg and CURRENT_STATE.CS_IDLE or CURRENT_STATE.CS_NOTREADY) end)
  end
  dbg("callstate", ev, tostring(msg.remoteAor or ""))
end

-- ── intercom proxy -> device (relayed) ──────────────────────────────────────
-- Our state for the proxy (CURRENT_STATE.*): busy in a call, else idle once
-- registered, else not-ready.
function IntercomCurrentState()
  if gInCall then return CURRENT_STATE.CS_BUSY end
  if gSipReg then return CURRENT_STATE.CS_IDLE end
  return CURRENT_STATE.CS_NOTREADY
end

-- Auto-answer is controlled by our own "Auto Answer" property (the native proxy
-- checkbox has NO SET command, so toggling it sends us nothing — confirmed). We
-- report this value in device state so the native checkbox reflects it. Default On.
-- Auto Answer / Monitor Mode / Play Door Chime follow the NATIVE proxy settings, polled
-- from our own GET_DEVICE props (see PollDeviceState). Default off until first poll;
-- Play Door Chime defaults ON (a keypad should chime for the door).
function autoAnswerOn()    return gProxyAuto == true end
function PlayDoorChimeOn() if gProxyChime == nil then return true end return gProxyChime end

-- Push live call behavior (auto-answer + monitor) to the device. Separate from the
-- SIP creds (`sip` msg) so toggling these does NOT re-register. Monitor mode =
-- silent auto-answer; Auto Answer = answer + heads-up beep; neither = ring.
function PushCallCfg()
  MMK_SendDevice({ t = "callcfg", autoAnswer = autoAnswerOn(),
                   monitor   = MonitorOn(),
                   playDoorChime = PlayDoorChimeOn(),
                   mute      = (INTERCOM_STATE and INTERCOM_STATE.muted) == true,
                   disableEc = (INTERCOM_STATE and INTERCOM_STATE.disableEc) == true })
end

-- Relay the live mic-mute to the device (MUTE_CALL handler calls this). Shares the
-- callcfg message so the device gets autoAnswer/monitor/mute in one place.
function PushMuteState() PushCallCfg() end

-- Play Door Chime: the proxy fires PLAY_DOOR_CHIME when a door-station call rings and the
-- "Play Door Chime" box is checked (has_play_door_chime). Sends the device a dedicated
-- `chime` message -> proper DOORBELL sound (descending ding-dong) + "Door" banner. (The
-- keypad relay forwards any json to :6700.)
function PlayDoorChime()
  MMK_SendDevice({ t = "chime", text = "Door" })
end

-- Immediately read + push the current ringer/speaker/mic levels to the device (called on
-- provision). Ongoing live updates come from the level POLL (StartLevelPoll below), because
-- C4's variable listener does NOT reliably fire OnWatchedVariableChanged for our OWN proxy
-- device — verified live: moving a slider produced no callback, so the device stayed at the
-- value pushed at provision. The poll reads the proxy's RINGER/SPEAKER/MICROPHONE_VOLUME
-- variables and forwards changes.
function PushLevels()
  gLastLevel = {}      -- force the next poll-read to (re)push all current values
  if PollLevels then PollLevels() end
end

-- Shared settings state the proxy SET_* commands mutate (intercom_command.lua) and
-- SendIntercomDeviceState reports back. Global so the bundled command handlers (a
-- separate file in the same Lua env) can reach it. cameraEnabled/sendVideo are
-- pinned false — we're an audio-only station. Levels + Monitor Mode now live in driver
-- Properties (auto-persisted by C4) because the intercomproxy never sends SET_* / config
-- commands to a third-party endpoint from Composer (ReceivedFromProxy never fires there —
-- verified live). INTERCOM_STATE only tracks runtime call state the proxy can't push.
INTERCOM_STATE = INTERCOM_STATE or {
  dnd = false, monitorMode = false, muted = false,
  cameraEnabled = false, sendVideo = false, excludeFromNav = false,
  disableEc = false,
}

-- ── Audio levels via PROXY VARIABLES (the real mechanism, verified live) ─────────────
-- Control4 stores the native Audio Control slider values as VARIABLES on our intercom
-- PROXY device — RINGER_VOLUME / SPEAKER_VOLUME / MICROPHONE_GAIN — NOT as SET_* commands.
-- So we READ them and register listeners; when a slider moves, OnWatchedVariableChanged
-- forwards the new value to the device. The proxy device id (e.g. 3163) differs from our
-- Lua device id; C4:GetProxyDevices() returns it.
local PROXY_LEVEL_NAMES = { RINGER_VOLUME = "ringer", SPEAKER_VOLUME = "speaker", MICROPHONE_GAIN = "mic" }
gProxyDev  = nil
LEVEL_VARS = {}     -- proxy variable id -> "ringer" / "speaker" / "mic"

function ProxyDeviceId()
  if gProxyDev then return gProxyDev end
  local ok, px = pcall(function() return C4:GetProxyDevices() end)
  gProxyDev = (ok and tonumber(tostring(px))) or nil
  return gProxyDev
end

-- Ask the intercom proxy (synchronously) for the full endpoint roster and push the set of
-- DOOR-STATION SIP usernames to the firmware. The firmware flags a call as a doorbell when
-- the caller is in this set (→ doorbell chime + manual answer, never auto-answer/monitor).
-- isDoorStation is Control4's own capability, so this is vendor-neutral — native DS2, the
-- Chowmain UniFi, 2N, etc. all resolve correctly. No peer-name heuristics on the device.
function RefreshDoorStations()
  local pid = ProxyDeviceId()
  if not pid then dbg("RefreshDoorStations: no proxy device id"); return end
  local ok, list = pcall(function() return C4:SendUIRequest(pid, "GET_DEVICE_LIST", {}) end)
  if not ok or type(list) ~= "string" then dbg("RefreshDoorStations: GET_DEVICE_LIST failed"); return end
  -- Also build the callable ENDPOINT list for the on-screen Intercom picker: every
  -- intercom endpoint except ourselves (rooms AND door stations — you can call a door
  -- to talk/see who's there). name = "Room - Device", user = SIP username, door flag.
  local me = (sipCreds())
  local users, endpoints = {}, {}
  for block in list:gmatch("<device_props>(.-)</device_props>") do
    local isDoor = block:match("<isDoorStation>(.-)</isDoorStation>")
    local user   = block:match("<sipUserName>(.-)</sipUserName>")
    local name   = block:match("<displayName>(.-)</displayName>") or ""
    local door   = (isDoor == "True" or isDoor == "true" or isDoor == "1")
    if user and user ~= "" then
      if door then users[#users + 1] = user end
      if user ~= me then
        name = (name ~= "" and name or user):gsub("`", " - ")
        -- Door endpoints advertise the action buttons the dealer configured. Only
        -- labelled ones are sent: the panel renders exactly what it is told and
        -- never infers an action from a target's name or kind.
        local acts = nil
        if door then
          acts = {}
          local l1 = tostring(Properties["Door Action 1 Label"] or "")
          local l2 = tostring(Properties["Door Action 2 Label"] or "")
          if l1 ~= "" then acts[#acts + 1] = { id = "1", label = l1 } end
          if l2 ~= "" then acts[#acts + 1] = { id = "2", label = l2 } end
          if #acts == 0 then acts = nil end
        end
        endpoints[#endpoints + 1] = { name = name, user = user, door = door, actions = acts }
      end
    end
  end
  -- GET_GROUP_LIST returns <group> blocks, each with a group <name>/<sipAOR> and a <devices>
  -- list of MEMBERS. Two kinds of callable target live here:
  --   * the GROUP itself (Everyone / call groups) — dialing its name broadcasts (RING_ALL;
  --     it has a sipAOR like Everyone@director);
  --   * MOBILE USERS ("Intercom Anywhere": Mike, Lorena, ...) that are group members — each is
  --     individually callable, but by its <user> (the SIP username, e.g. mike.delucafamily.org),
  --     NOT its display <name>. (The old greedy <name> scan grabbed member display names and
  --     dialed them as groups -> sip:Mike@director -> 404.)
  local groups, mobiles = {}, {}
  local okg, glist = pcall(function() return C4:SendUIRequest(pid, "GET_GROUP_LIST", {}) end)
  if okg and type(glist) == "string" then
    for gblock in glist:gmatch("<group>(.-)</group>") do
      local head  = gblock:match("^(.-)<devices>") or gblock         -- group-level fields only
      local gname = head:match("<name>(.-)</name>")
      if gname and gname ~= "" and gname ~= "All" then groups[gname] = true end
      for dev in gblock:gmatch("<device>(.-)</device>") do
        if (dev:match("<isMobileUser>(.-)</isMobileUser>") or ""):lower() == "true" then
          local u  = dev:match("<user>(.-)</user>")
          local nm = dev:match("<intercomname>(.-)</intercomname>")
          nm = (nm and nm ~= "") and nm or (dev:match("<name>(.-)</name>") or u)
          if u and u ~= "" and u ~= me then mobiles[u] = (nm or u):gsub("`", " - ") end
        end
      end
    end
  end
  groups["Everyone"] = true                          -- standard local broadcast
  local targets = {}
  local ngroups = 0
  for gname in pairs(groups) do targets[#targets + 1] = { name = gname, user = gname, group = true }; ngroups = ngroups + 1 end
  table.sort(targets, function(a, b) return a.name < b.name end)   -- groups first, alpha
  local mlist = {}
  -- mobile=true so the panel can show these with a phone glyph rather than the
  -- generic room mark -- they are people, not rooms (the Control4 app does the same).
  for u, nm in pairs(mobiles) do mlist[#mlist + 1] = { name = nm, user = u, mobile = true } end  -- dial by <user>
  table.sort(mlist, function(a, b) return a.name < b.name end)
  for _, e in ipairs(mlist) do targets[#targets + 1] = e end
  for _, e in ipairs(endpoints) do targets[#targets + 1] = e end
  dbg(string.format("RefreshDoorStations: %d door(s), %d group(s), %d mobile(s), %d endpoint(s)",
      #users, ngroups, #mlist, #endpoints))
  MMK_SendDevice({ t = "doorstations", users = users })
  MMK_SendDevice({ t = "endpoints", list = targets })
end

local function clamp100(v)
  v = tonumber(v); if not v then return nil end
  if v < 0 then v = 0 elseif v > 100 then v = 100 end
  return math.floor(v + 0.5)
end

-- Forward one level to the device. Global so the proxy SET_* handlers can reuse it.
function PushOneLevel(kind, val)
  local n = clamp100(val); if not n then return end
  if     kind == "ringer"  then PROTOCOL.SET_RINGER_VOLUME({ value = n })
  elseif kind == "speaker" then PROTOCOL.SET_SPEAKER_VOLUME({ value = n })
  elseif kind == "mic"     then PROTOCOL.SET_MICROPHONE_GAIN({ value = n }) end
end

-- Map the proxy's level variable NAMES -> ids and register change listeners. Idempotent;
-- safe to retry until the proxy device id / variables are available.
function BindProxyLevelVars()
  local px = ProxyDeviceId(); if not px then dbg("proxy device id not ready"); return false end
  LEVEL_VARS = {}
  local ok, vars = pcall(function() return C4:GetDeviceVariables(px) end)
  if ok and type(vars) == "table" then
    for id, t in pairs(vars) do
      local name = (type(t) == "table") and t.name
      local kind = name and PROXY_LEVEL_NAMES[name]
      if kind then
        LEVEL_VARS[id] = kind
        pcall(function() C4:RegisterVariableListener(px, id) end)
        dbg("watching proxy level var", tostring(id), tostring(name))
      end
    end
  end
  return next(LEVEL_VARS) ~= nil
end

-- A native Audio Control slider moved -> its proxy variable changed -> forward to device.
-- (Kept as a bonus; in practice it does NOT fire for our own proxy device — the poll below
-- is what actually carries slider changes to the device.)
function OnWatchedVariableChanged(idDevice, idVariable, strValue)
  local kind = LEVEL_VARS[idVariable]
  if kind then dbg("level(listener)", kind, "=", tostring(strValue)); PushOneLevel(kind, strValue) end
end

-- ── Level POLL (the mechanism that actually works) ───────────────────────────────────
-- The self-proxy variable listener doesn't deliver OnWatchedVariableChanged, so read the
-- RINGER/SPEAKER/MICROPHONE_VOLUME proxy variables on a 2s timer and push any change to the
-- device. gLastLevel dedupes so we only send sipvol when a value actually moves.
gLevelTimer = nil
gLastLevel  = {}     -- kind -> last value pushed

-- An intercom proxy that has never had its sliders touched reports 0 for every level.
-- Forwarding that verbatim MUTED the panel: speaker=0 killed the selftest tone and the
-- mic->speaker loopback, and ringer=0 killed the chime (the "toggle Mute off and on to
-- get sound back" workaround was just re-applying the device's own saved setting).
-- 0 is a legitimate value once a dealer has actually set it, so we only seed a sane
-- default the FIRST time we see an untouched (0) level -- after that 0 is honoured.
local LEVEL_DEFAULT = { ringer = 75, speaker = 75, mic = 75 }
gLevelSeeded = gLevelSeeded or {}

-- Push a level INTO the proxy. C4:SetVariable on the proxy's own level variable does
-- NOT hold -- the proxy owns those variables and overwrites the write on its next
-- pass (observed: 75 accepted, back to 0 within one 2s poll). The supported route is
-- the proxy's notification contract, which makes the proxy update its variable (and
-- the Composer slider) itself.
local function notify_level(kind, n)
  if     kind == "ringer"  then NOTIFY.Ringer_Volume_Changed(n)
  elseif kind == "speaker" then NOTIFY.Speaker_Volume_Changed(n)
  elseif kind == "mic"     then NOTIFY.Microphone_Gain_Changed(n) end
end

function PollLevels()
  local px = ProxyDeviceId(); if not px then return end
  if not next(LEVEL_VARS) then BindProxyLevelVars() end
  for id, kind in pairs(LEVEL_VARS) do
    local ok, v = pcall(function() return C4:GetDeviceVariable(px, id) end)
    local n = ok and clamp100(v)
    -- NEVER forward 0. The proxy reports 0 when a slider has never been set, but
    -- also -- observed live on every instance -- spontaneously afterwards, e.g.
    -- around call setup. Seeding only on the FIRST 0 meant every later 0 was
    -- pushed straight through, which silences the panel's ringer/speaker/mic
    -- until somebody moves a slider in Composer. 0 is never a level worth
    -- sending: muting is a separate control (Mute_Audio_Changed). Re-seed the
    -- proxy variable with the default instead, every time.
    if n == 0 or n == nil then
      n = LEVEL_DEFAULT[kind] or 75
      if not gLevelSeeded[kind] then
        dbg("level", kind, "was unset/0 -> seeding default", n)
      end
      gLevelSeeded[kind] = true
      pcall(function() notify_level(kind, n) end)
    else
      gLevelSeeded[kind] = true
    end
    if n and gLastLevel[kind] ~= n then
      gLastLevel[kind] = n
      PushOneLevel(kind, n)
      dbg("level", kind, "=", n)
    end
  end
end

function StopLevelPoll()
  if gLevelTimer then
    pcall(function() if type(gLevelTimer) == "table" then gLevelTimer:Cancel() else C4:KillTimer(gLevelTimer) end end)
    gLevelTimer = nil
  end
end

-- Levels + proxy-config flags have no change event, so we poll them — but they change
-- rarely, so being a good neighbor at 20 keypads means NOT hammering every 2s forever.
-- Poll fast only during an active call (where mid-call slider tweaks want to land
-- promptly); back well off when idle. Self-reschedules so the rate follows gInCall.
local LEVEL_POLL_FAST = 2000
local LEVEL_POLL_IDLE = 15000
function StartLevelPoll()
  StopLevelPoll()
  local function tick()
    pcall(PollLevels); pcall(PollDeviceState)
    gLevelTimer = C4:SetTimer(gInCall and LEVEL_POLL_FAST or LEVEL_POLL_IDLE, tick, false)
  end
  gLevelTimer = C4:SetTimer(LEVEL_POLL_FAST, tick, false)
end

function MonitorOn() return gProxyMon == true end

-- Poll our OWN native proxy settings (Auto Answer / Monitor Mode / Play Door Chime) from
-- our GET_DEVICE props and push changes to the device. These live only on the proxy (no
-- reliable SET_* command reaches the driver), so we read them like the volume levels.
function PollDeviceState()
  local pid = ProxyDeviceId(); if not pid then return end
  local ok, props = pcall(function() return C4:SendUIRequest(pid, "GET_DEVICE", { deviceId = pid }) end)
  if not ok or type(props) ~= "string" then return end
  local function b(tag)
    local v = props:match("<" .. tag .. ">(.-)</" .. tag .. ">")
    if v == nil then return nil end
    return (v == "True" or v == "true" or v == "1")
  end
  local aa, mm, ch = b("autoAnswer"), b("monitorMode"), b("playDoorChime")
  if aa == nil and mm == nil and ch == nil then return end
  local changed = (aa ~= gProxyAuto) or (mm ~= gProxyMon) or (ch ~= gProxyChime)
  if aa ~= nil then gProxyAuto = aa end
  if mm ~= nil then gProxyMon = mm; if INTERCOM_STATE then INTERCOM_STATE.monitorMode = mm end end
  if ch ~= nil then gProxyChime = ch end
  if changed then
    dbg(string.format("proxy cfg: autoAnswer=%s monitor=%s doorChime=%s",
        tostring(gProxyAuto), tostring(gProxyMon), tostring(gProxyChime)))
    PushCallCfg()
  end
end

-- Reply to the proxy's GET_STATE / GET_STATE_LIST. (Volumes/gain best-effort
-- defaults — LIVE-VALIDATE exact keys on a Director.)
function SendIntercomDeviceState()
  NOTIFY.Device_State_Changed({
    deviceId       = selfDeviceId(),
    currentState   = IntercomCurrentState(),
    autoAnswer     = autoAnswerOn(),
    dndSetting     = INTERCOM_STATE.dnd,
    monitorMode    = MonitorOn(),
    excludeFromNav = INTERCOM_STATE.excludeFromNav,
    cameraEnabled  = false,   -- audio-only station
    sendVideo      = false,   -- audio-only station
    -- ringer/speaker/mic are the proxy's OWN variables (RINGER_VOLUME/SPEAKER_VOLUME/
    -- MICROPHONE_GAIN) — we read+forward them, we don't echo them back here.
  })
end

-- SIP server = the Director (the internal Communication Server / FreeSWITCH runs
-- there — matches the proxy's IP Address field). Nothing settable.
function sipServer()
  local ok, ctrl = pcall(function() return C4:GetControllerNetworkAddress() end)
  return (ok and ctrl and tostring(ctrl)) or ""
end

-- Auto-generated SIP account (no dealer entry). Username embeds our C4 device id
-- (like UniFi_<id>_xxx); password is generated once and PERSISTED so it's stable
-- across reloads (Director's directory + the device must keep agreeing). We report
-- both to the proxy (SIP_USERNAME_CHANGED → Director provisions its xml_curl
-- directory) AND push both to the device, so the internal Communication Server
-- authenticates the device's registration. No "SIP Username/Password" properties.
function sipCreds()
  local id = selfDeviceId()
  local user = "MMKeypad_" .. tostring(id)
  local ok, v = pcall(function() return C4:PersistGetValue("sip_pass") end)
  local pass = (ok and v and tostring(v) ~= "") and tostring(v) or nil
  if not pass then
    pcall(function() math.randomseed((os.time and os.time() or 0) + id * 131) end)
    local chars = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789"
    pass = ""
    for _ = 1, 14 do local n = math.random(1, #chars); pass = pass .. chars:sub(n, n) end
    pcall(function() C4:PersistSetValue("sip_pass", pass) end)
  end
  return user, pass
end

-- Answer the proxy's GET_DEVICE with our device props so the native SIP Information
-- panel populates (User Name / Display Name / sipAOR) — like the UniFi/door stations.
-- [LIVE-VALIDATE the response shape on a Director; best-effort per the proxy docs.]
function SendDeviceProps()
  local me   = selfDeviceId()
  local user = (sipCreds())
  local srv  = sipServer()
  local aor  = (user ~= "" and srv ~= "") and (user .. "@" .. srv) or ""
  pcall(function() C4:SendToProxy(INTERCOM_PROXY, "GET_DEVICE", {
    proxyId = me, protocolId = me,
    hasCamera = 0, hasDisplay = 0, hasButtons = 0, isDoorStation = 0,
    sipUserName = user, sipAOR = aor,
  }, "NOTIFY") end)
end

-- Provision the auto-generated SIP account: report it to the proxy (→ Director) and
-- push it to the device (via the relay). Called on link-up. No dealer entry needed.
function PushSipConfig()
  local user, pass = sipCreds()
  local server = sipServer()
  NOTIFY.Sip_Username_Changed(user, pass)   -- → Director registers the account
  SendDeviceProps()
  if server == "" then
    pcall(function() C4:UpdateProperty("SIP Registration", "No Director address") end)
    return
  end
  if not gKeypad then dbg("PushSipConfig: no keypad link yet"); return end
  -- Display name for SIP. Without it the far end has only the AOR to show, so a
  -- call from the kitchen panel announced itself as "MMKeypad_3414". A room IS a
  -- device in Control4, so its display name is the room name ("Kitchen").
  local dname = ""
  pcall(function()
    local rid = C4:RoomGetId()
    if rid then dname = tostring(C4:GetDeviceDisplayName(rid) or "") end
  end)
  if dname == "" then pcall(function()
    dname = tostring(C4:GetDeviceDisplayName(selfDeviceId()) or "")
  end) end

  PROTOCOL.PUSH_SIP({
    server    = server,
    port      = 5060,
    transport = SIP_TRANSPORT,   -- fixed; see top of file
    user      = user,
    pass      = pass,
    name      = dname,
    autoAnswer = autoAnswerOn(),
  })
  PushCallCfg()                  -- auto-answer / monitor / mute (live, no re-register)
  PushLevels()                   -- restore persisted ringer/speaker/mic on the device
  RefreshDoorStations()          -- push the door-station set so the device flags doorbells
  dbg("provisioned SIP:", user .. "@" .. server)
end

-- All intercom proxy commands arrive here (binding 5001) -> bundled COMMAND_HANDLER.
function ReceivedFromProxy(idBinding, strCommand, tParams)
  -- DIAG (log BEFORE the binding filter): if the proxy ever commands us at all, this
  -- prints — even if it arrives on an unexpected binding id. If sliders move and this
  -- never prints, the proxy isn't delivering commands to this driver.
  dbg("ReceivedFromProxy: binding", tostring(idBinding), "cmd", tostring(strCommand))
  if idBinding ~= INTERCOM_PROXY then return end
  tParams = tParams or {}
  -- State queries: the proxy probes state on init; the driver MUST answer or the
  -- endpoint stays NotReady (current intercom proxy-protocol docs).
  if strCommand == "GET_CURRENT_STATE" then
    NOTIFY.Current_State_Changed(IntercomCurrentState())
  elseif strCommand == "GET_STATE" or strCommand == "GET_STATE_LIST" then
    SendIntercomDeviceState()
  elseif strCommand == "GET_DEVICE" or strCommand == "GET_DEVICE_LIST" then
    -- The proxy asks for our device props (SIP user / AOR / camera-display flags)
    -- to populate the native SIP Information panel. Answer with SendDeviceProps.
    SendDeviceProps()
  else
    local handler = COMMAND_HANDLER[strCommand]
    if handler then pcall(handler, tParams, idBinding)
    else dbg("ignoring intercom proxy cmd:", tostring(strCommand)) end
  end
end
