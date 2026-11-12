-- Simulated MGSV + Dynamite for testing run_qa.py without the game (run inside qabridge_host).
-- Two processes (host and client) share a folder to fake the network. Timings are in frames (10 ms).
--
-- env: QA_SIM_ROLE=host|client, QA_SIM_SHARED=<dir>
--      QA_SIM_DESYNC=1     client sees the host 5 m off (positions test must fail)
--      QA_SIM_NOCONNECT=1  CreateClientSession always fails (connect must fail)
--      QA_SIM_CRASH_IN_MISSION=1  the client "crashes" 200 frames into the mission

local role = os.getenv("QA_SIM_ROLE") or "host"
local shared = os.getenv("QA_SIM_SHARED") or "."
local frame = 0
local timers = {}

local function exists(p)
	local f = io.open(p, "r")
	if f then
		f:close()
		return true
	end
	return false
end
local function touch(p)
	local f = io.open(p, "w")
	f:write("1")
	f:close()
end

os.execute("mkdir -p dynamite")
if role == "host" then -- a new host is a new session: forget the previous one
	os.remove(shared .. "/connected")
	os.remove(shared .. "/host_session")
end
local logf = io.open("dynamite/dynamite.log.txt", "w") -- the real log is truncated at game start too
local function log(s)
	logf:write("[sim] [dynamite] [info] " .. s .. "\n")
	logf:flush()
end
local function after(n, f)
	timers[#timers + 1] = { at = frame + n, f = f }
end

vars = { missionCode = 1, playerPosX = 0, playerPosY = 0, playerPosZ = 0, playerLife = 6000 }
TppDefine = { MISSION_CLEAR_TYPE = { FROM_HELISPACE = 7 }, STORY_MISSION_CLUSTER_ID = { [10020] = 3 } }
-- each copy has its own save unless QA_SIM_SHARED_SAVE (the bug seen on 2026-11-09)
TppDefine.GAME_SAVE_FILE_NAME = (role == "client" and not os.getenv("QA_SIM_SHARED_SAVE")) and "DYN2_GAME_DATA" or "DYNAMITE_GAME_DATA"
TppMission = {
	IsHelicopterSpace = function(c)
		return c == 40010 or c == 40020
	end,
	ReserveMissionClear = function(t)
		vars.missionCode = 0 -- loading
		after(150, function()
			vars.missionCode = t.nextMissionId
			log("[lua] TppMission.OnMissionStart, mission code " .. t.nextMissionId)
			if role == "host" then
				log("int Dynamite::l_CreateHostSession(lua_State *), host session res: 1")
				touch(shared .. "/host_session")
			elseif os.getenv("QA_SIM_CRASH_IN_MISSION") then
				after(200, function()
					QA_SIM_EXIT = 3
				end)
			end
		end)
		return true
	end,
}

local positions = { [0] = { 100, 10, 200 }, [1] = { 105, 10, 203 } }
Dynamite = {
	IsHost = function()
		return role == "host"
	end,
	IsClient = function()
		return role == "client"
	end,
	IsSynchronized = function()
		return exists(shared .. "/connected")
	end,
	QAMemberCount = function()
		return exists(shared .. "/connected") and 2 or 1
	end,
	QAPlayerPos = function(i)
		local p = positions[i]
		local off = (role == "client" and i == 0 and os.getenv("QA_SIM_DESYNC")) and 5 or 0
		return p[1] + off, p[2], p[3]
	end,
	CreateClientSession = function()
		log("int Dynamite::l_CreateClientSession(lua_State *)")
		if os.getenv("QA_SIM_NOCONNECT") or not exists(shared .. "/host_session") then
			log("dynamite_connection_failed")
			return
		end
		after(50, function()
			touch(shared .. "/connected")
			log("void Dynamite::UpdateClientEstablishedHook(void *): Co-op connection established")
		end)
	end,
}

QA_HOST = { freezeMs = 0, quit = false, stackErrors = 0 }

after(100, function() -- title -> helicopter happens by itself here (the owner presses Continue in the game)
	vars.missionCode = 40010
	log("[lua] TppMission.OnMissionStart, mission code 40010")
end)

function QA_SIM_TICK()
	frame = frame + 1
	local due, keep = {}, {}
	for _, t in ipairs(timers) do
		if t.at <= frame then
			due[#due + 1] = t
		else
			keep[#keep + 1] = t
		end
	end
	timers = keep -- timers added by a due callback go into this new list
	for _, t in ipairs(due) do
		t.f()
	end
end

-- for qa/probe_title.py
Fox = { StrCode32 = function(s) return #s * 1000 + string.byte(s) end }
OnlineChallengeTask = { OnMessage = function() end }
