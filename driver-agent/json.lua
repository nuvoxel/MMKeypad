--
-- json.lua — a lightweight JSON library for Lua (MIT)
-- Based on rxi/json.lua (https://github.com/rxi/json.lua).
-- Bundled so the driver does not depend on C4:JsonEncode/Decode (which have
-- quirky array/primitive semantics). See ../../PROTOCOL.md.
--

local json = { _version = "0.1.2" }

-------------------------------------------------------------------------------
-- Encode
-------------------------------------------------------------------------------

local encode

local escape_char_map = {
  [ "\\" ] = "\\", [ "\"" ] = "\"", [ "\b" ] = "b",
  [ "\f" ] = "f",  [ "\n" ] = "n",  [ "\r" ] = "r", [ "\t" ] = "t",
}

local escape_char_map_inv = { [ "/" ] = "/" }
for k, v in pairs(escape_char_map) do escape_char_map_inv[v] = k end

local function escape_char(c)
  return "\\" .. (escape_char_map[c] or string.format("u%04x", c:byte()))
end

local function encode_nil() return "null" end

local function encode_table(val, stack)
  local res = {}
  stack = stack or {}
  if stack[val] then error("circular reference") end
  stack[val] = true

  if rawget(val, 1) ~= nil or next(val) == nil then
    -- treat as array
    local n = 0
    for k in pairs(val) do
      if type(k) ~= "number" then error("invalid table: mixed or invalid key types") end
      n = n + 1
    end
    if n ~= #val then error("invalid table: sparse array") end
    for _, v in ipairs(val) do table.insert(res, encode(v, stack)) end
    stack[val] = nil
    return "[" .. table.concat(res, ",") .. "]"
  else
    -- treat as object
    for k, v in pairs(val) do
      if type(k) ~= "string" then error("invalid table: mixed or invalid key types") end
      table.insert(res, encode(k, stack) .. ":" .. encode(v, stack))
    end
    stack[val] = nil
    return "{" .. table.concat(res, ",") .. "}"
  end
end

local function encode_string(val)
  return '"' .. val:gsub('[%z\1-\31\\"]', escape_char) .. '"'
end

local function encode_number(val)
  if val ~= val or val <= -math.huge or val >= math.huge then
    error("unexpected number value '" .. tostring(val) .. "'")
  end
  return string.format("%.14g", val)
end

local type_func_map = {
  [ "nil"     ] = encode_nil,
  [ "table"   ] = encode_table,
  [ "string"  ] = encode_string,
  [ "number"  ] = encode_number,
  [ "boolean" ] = tostring,
}

encode = function(val, stack)
  local f = type_func_map[type(val)]
  if f then return f(val, stack) end
  error("unexpected type '" .. type(val) .. "'")
end

function json.encode(val)
  return encode(val)
end

-------------------------------------------------------------------------------
-- Decode
-------------------------------------------------------------------------------

local parse

local function create_set(...)
  local res = {}
  for i = 1, select("#", ...) do res[ select(i, ...) ] = true end
  return res
end

local space_chars   = create_set(" ", "\t", "\r", "\n")
local delim_chars   = create_set(" ", "\t", "\r", "\n", "]", "}", ",")
local escape_chars  = create_set("\\", "/", '"', "b", "f", "n", "r", "t", "u")
local literals      = create_set("true", "false", "null")
local literal_map   = { ["true"] = true, ["false"] = false, ["null"] = nil }

local function next_char(str, idx, set, negate)
  for i = idx, #str do
    if set[str:sub(i, i)] ~= negate then return i end
  end
  return #str + 1
end

local function decode_error(str, idx, msg)
  error(string.format("%s at position %d", msg, idx))
end

local function parse_string(str, i)
  local has_unicode_escape, res = false, {}
  local j = i + 1
  while j <= #str do
    local x = str:byte(j)
    if x < 32 then
      decode_error(str, j, "control character in string")
    elseif x == 92 then -- backslash
      local c = str:sub(j + 1, j + 1)
      if c == "u" then
        local hex = str:sub(j + 2):match("^[dD][89aAbB]%x%x\\u%x%x%x%x") or
                    str:sub(j + 2):match("^%x%x%x%x")
        if not hex then decode_error(str, j, "invalid unicode escape") end
        -- keep escapes verbatim; titles rarely need full decode for our use
        table.insert(res, str:sub(j, j + 1 + #hex))
        j = j + 2 + #hex
      else
        if not escape_chars[c] then decode_error(str, j, "invalid escape '\\" .. c .. "'") end
        local map = { b="\b", f="\f", n="\n", r="\r", t="\t" }
        table.insert(res, map[c] or c)
        j = j + 2
      end
    elseif x == 34 then -- double quote
      return table.concat(res), j + 1
    else
      table.insert(res, str:sub(j, j))
      j = j + 1
    end
  end
  decode_error(str, i, "expected closing quote for string")
end

local function parse_number(str, i)
  local x = next_char(str, i, delim_chars)
  local s = str:sub(i, x - 1)
  local n = tonumber(s)
  if not n then decode_error(str, i, "invalid number '" .. s .. "'") end
  return n, x
end

local function parse_literal(str, i)
  local x = next_char(str, i, delim_chars)
  local word = str:sub(i, x - 1)
  if not literals[word] then decode_error(str, i, "invalid literal '" .. word .. "'") end
  return literal_map[word], x
end

local function parse_array(str, i)
  local res = {}
  i = i + 1
  while true do
    i = next_char(str, i, space_chars, true)
    if str:sub(i, i) == "]" then return res, i + 1 end
    local v
    v, i = parse(str, i)
    table.insert(res, v)
    i = next_char(str, i, space_chars, true)
    local chr = str:sub(i, i)
    i = i + 1
    if chr == "]" then return res, i end
    if chr ~= "," then decode_error(str, i, "expected ']' or ','") end
  end
end

local function parse_object(str, i)
  local res = {}
  i = i + 1
  while true do
    i = next_char(str, i, space_chars, true)
    if str:sub(i, i) == "}" then return res, i + 1 end
    if str:sub(i, i) ~= '"' then decode_error(str, i, "expected string key") end
    local key
    key, i = parse(str, i)
    i = next_char(str, i, space_chars, true)
    if str:sub(i, i) ~= ":" then decode_error(str, i, "expected ':'") end
    local val
    val, i = parse(str, next_char(str, i + 1, space_chars, true))
    res[key] = val
    i = next_char(str, i, space_chars, true)
    local chr = str:sub(i, i)
    i = i + 1
    if chr == "}" then return res, i end
    if chr ~= "," then decode_error(str, i, "expected '}' or ','") end
  end
end

local char_func_map = {
  [ '"' ] = parse_string,
  [ "0" ] = parse_number, [ "1" ] = parse_number, [ "2" ] = parse_number,
  [ "3" ] = parse_number, [ "4" ] = parse_number, [ "5" ] = parse_number,
  [ "6" ] = parse_number, [ "7" ] = parse_number, [ "8" ] = parse_number,
  [ "9" ] = parse_number, [ "-" ] = parse_number,
  [ "t" ] = parse_literal, [ "f" ] = parse_literal, [ "n" ] = parse_literal,
  [ "[" ] = parse_array,   [ "{" ] = parse_object,
}

parse = function(str, idx)
  local chr = str:sub(idx, idx)
  local f = char_func_map[chr]
  if f then return f(str, idx) end
  decode_error(str, idx, "unexpected character '" .. chr .. "'")
end

function json.decode(str)
  if type(str) ~= "string" then error("expected argument of type string") end
  local res, idx = parse(str, next_char(str, 1, space_chars, true))
  idx = next_char(str, idx, space_chars, true)
  if idx <= #str then decode_error(str, idx, "trailing garbage") end
  return res
end

return json
