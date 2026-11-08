-- coopqa: test-only bot bridge for Dynamite (coop-anything bridge protocol).
--
-- Loaded by script_loader only when <game dir>\coopqa.ini exists. Each frame Update() hands the
-- dispatcher to Dynamite.QAPump, which runs pending bridge requests on this (the game's) Lua thread.
-- The runner (qa/run_qa.py) sends Lua through `exec`/`eval` and reads `state`.
--
-- Contract with hook/src/QABridge.cpp: dispatch(kind, arg) never raises and returns a string that
-- starts with "1" + JSON (ok) or "0" + message (error).

local M = {}

M.frame = 0
M.version = 1

-- ---------------------------------------------------------------- JSON (encode only)
local function esc(s)
	s = string.gsub(s, '[%c"\\]', function(c)
		if c == '"' then
			return '\\"'
		elseif c == "\\" then
			return "\\\\"
		elseif c == "\n" then
			return "\\n"
		elseif c == "\r" then
			return "\\r"
		elseif c == "\t" then
			return "\\t"
		end
		return string.format("\\u%04x", string.byte(c))
	end)
	return '"' .. s .. '"'
end

local function isArray(t)
	local n = 0
	for k, _ in pairs(t) do
		if type(k) ~= "number" or k < 1 or math.floor(k) ~= k then
			return false
		end
		n = n + 1
	end
	for i = 1, n do
		if t[i] == nil then
			return false
		end
	end
	return true, n
end

local function encode(v, depth)
	depth = depth or 0
	if depth > 20 then
		return '"<too deep>"'
	end
	local t = type(v)
	if v == nil then
		return "null"
	elseif t == "boolean" then
		return v and "true" or "false"
	elseif t == "number" then
		if v ~= v or v == math.huge or v == -math.huge then
			return "null"
		end
		if math.floor(v) == v and math.abs(v) < 1e15 then
			return string.format("%d", v)
		end
		return string.format("%.6f", v)
	elseif t == "string" then
		return esc(v)
	elseif t == "table" then
		local arr, n = isArray(v)
		local parts = {}
		if arr then -- an empty table is encoded as [] (Lua can't tell [] from {})
			for i = 1, n do
				parts[#parts + 1] = encode(v[i], depth + 1)
			end
			return "[" .. table.concat(parts, ",") .. "]"
		end
		for k, x in pairs(v) do
			parts[#parts + 1] = esc(tostring(k)) .. ":" .. encode(x, depth + 1)
		end
		return "{" .. table.concat(parts, ",") .. "}"
	end
	return esc(tostring(v))
end
M.encode = encode

-- ---------------------------------------------------------------- state
local function try(f, ...)
	if type(f) ~= "function" then
		return nil
	end
	local ok, r = pcall(f, ...)
	if ok then
		return r
	end
	return nil
end

-- Player positions come from PlayerInfoService, which hung the game on the title screen
-- (1.0.15.4, 2026-11-09). Only ask inside a real mission: not the title (1), not helicopter space.
function M.inMission()
	if not vars or not vars.missionCode then
		return false
	end
	local c = vars.missionCode
	if c < 10000 then -- 1 = init/title; story missions are 10xxx, free roam 30xxx, heli 40xxx
		return false
	end
	if TppMission and TppMission.IsHelicopterSpace and TppMission.IsHelicopterSpace(c) then
		return false
	end
	return true
end

M.traced = {}
local function trace(step)
	-- the first state() in each mission logs its steps, so a hang shows where it stopped
	local key = vars and vars.missionCode or -1
	if M.traced[key] == "done" then
		return
	end
	if Dynamite and Dynamite.Log then
		pcall(Dynamite.Log, "coopqa: state step " .. step .. " (mission " .. tostring(key) .. ")")
	end
end

function M.players()
	if not M.inMission() then
		error("players are only available inside a mission (mission " .. tostring(vars and vars.missionCode) .. ")", 0)
	end
	local out = {}
	local n = try(Dynamite.QAMemberCount) or 1
	for i = 0, n - 1 do
		local ok, x, y, z = pcall(Dynamite.QAPlayerPos, i)
		if ok then
			out[#out + 1] = { index = i, x = x, y = y, z = z }
		end
	end
	return out
end

function M.state(sections)
	local want = {}
	for _, name in ipairs(sections or {}) do
		want[name] = true
	end
	local s = {}
	trace("begin")
	s.qaVersion = M.version
	s.frame = M.frame
	s.mission = vars and vars.missionCode or nil
	trace("dynamite")
	s.isHost = try(Dynamite.IsHost)
	s.isClient = try(Dynamite.IsClient)
	s.members = try(Dynamite.QAMemberCount)
	s.synchronized = try(Dynamite.IsSynchronized)
	if TppNetworkUtil then
		s.sessionConnect = try(TppNetworkUtil.IsSessionConnect)
	end
	if TppMission and vars then
		s.heliSpace = try(TppMission.IsHelicopterSpace, vars.missionCode)
	end
	s.inMission = M.inMission()
	if want.players then
		trace("players")
		s.players = M.players() -- raises outside a mission: the caller asked for something unsafe
	end
	trace("vars")
	if vars then
		s.self = { x = vars.playerPosX, y = vars.playerPosY, z = vars.playerPosZ, life = vars.playerLife }
	end
	trace("end")
	M.traced[vars and vars.missionCode or -1] = "done"
	return s
end

-- ---------------------------------------------------------------- actions the runner calls by name
-- From the helicopter (ACC): start a mission the way the deploy menu does.
function M.startMission(missionCode)
	if not TppMission.IsHelicopterSpace(vars.missionCode) then
		return { ok = false, reason = "not in helicopter space", mission = vars.missionCode }
	end
	local r = TppMission.ReserveMissionClear({
		missionClearType = TppDefine.MISSION_CLEAR_TYPE.FROM_HELISPACE,
		nextMissionId = missionCode,
		nextClusterId = TppDefine.STORY_MISSION_CLUSTER_ID[missionCode],
	})
	return { ok = r ~= false, reserved = r }
end

function M.connect()
	if Dynamite.IsHost() then
		return { ok = false, reason = "host does not connect" }
	end
	Dynamite.CreateClientSession()
	return { ok = true }
end

-- ---------------------------------------------------------------- dispatcher
local function run(kind, arg)
	if kind == "state" then
		local sections = {}
		for name in string.gmatch(arg or "", "[^,]+") do
			sections[#sections + 1] = name
		end
		return encode(M.state(sections))
	elseif kind == "eval" then
		local f, err = loadstring("return " .. arg, "=qa.eval")
		if not f then
			error("compile: " .. tostring(err), 0)
		end
		return encode(f())
	elseif kind == "exec" then
		local f, err = loadstring(arg, "=qa.exec")
		if not f then
			error("compile: " .. tostring(err), 0)
		end
		return encode({ f() })
	end
	error("unknown kind " .. tostring(kind), 0)
end

M.dispatchLogged = {}
function M.dispatch(kind, arg)
	local ok, res = pcall(run, kind, arg)
	local key = vars and vars.missionCode or -1
	if not M.dispatchLogged[key] and Dynamite and Dynamite.Log then
		M.dispatchLogged[key] = true
		pcall(Dynamite.Log, "coopqa: first reply in mission " .. tostring(key) .. " (" .. tostring(kind) .. ", ok=" .. tostring(ok) .. ")")
	end
	if ok then
		return "1" .. tostring(res)
	end
	return "0" .. tostring(res)
end

-- The bridge is pumped only from the helicopter and missions (mission code >= 10000). On the
-- title screen (mission 1) the game hung on 2026-11-09 right after the first served request,
-- twice; cause not known yet. The runner just waits until the bridge answers.
M.pumpLogged = {}
function M.Update()
	M.frame = M.frame + 1
	local code = vars and vars.missionCode or 0
	if code < 10000 then
		return
	end
	if not M.pumpLogged[code] and Dynamite and Dynamite.Log then
		M.pumpLogged[code] = true
		pcall(Dynamite.Log, "coopqa: pumping in mission " .. tostring(code))
	end
	if Dynamite and Dynamite.QAPump then
		Dynamite.QAPump(M.dispatch)
	end
end

-- reachable from exec as CoopQA.*
_G.CoopQA = M

return M
