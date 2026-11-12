"""What do the two games know about D-Horse? Observation only, plus one buddy call per side.

Order: API names and state on both sides; positions before; the HOST calls D-Horse next to itself, 25 s of
samples from both sides; the CLIENT calls D-Horse, 25 s of samples. Every Lua call is wrapped: an API that
does not exist is recorded as an error string, not raised.
"""

import time

LUA_LIB = r"""
CoopQA.x = {}
local X = CoopQA.x
local function try(f, ...)
	local r = { pcall(f, ...) }
	if r[1] then
		return r[2]
	end
	return "<err " .. tostring(r[2]) .. ">"
end
local function v3(p)
	local ok, x, y, z = pcall(function() return p:GetX(), p:GetY(), p:GetZ() end)
	if ok then
		return { x = x, y = y, z = z }
	end
	return "<" .. type(p) .. " " .. tostring(p) .. ">"
end
function X.keys(name)
	local t = getfenv(0)[name] or _G[name]
	if type(t) ~= "table" then
		return "<" .. type(t) .. ">"
	end
	local out = {}
	for k, v in pairs(t) do
		out[#out + 1] = tostring(k) .. ":" .. (type(v) == "function" and "f" or tostring(v))
	end
	table.sort(out)
	return out
end
function X.objects(typeName, n)
	local out = {}
	for i = 0, n - 1 do
		local id = try(function() return GameObject.GetGameObjectIdByIndex(typeName, i) end)
		local e = { index = i, id = id }
		if type(id) == "number" and id ~= try(function() return GameObject.NULL_ID end) then
			local pos = try(function() return GameObject.SendCommand(id, { id = "GetPosition" }) end)
			e.pos = type(pos) == "string" and pos or v3(pos)
		end
		out[#out + 1] = e
	end
	return out
end
function X.snap()
	return {
		frame = CoopQA.frame,
		mission = vars.missionCode,
		buddyType = try(function() return vars.buddyType end),
		vehicleId = try(function() return vars.playerVehicleGameObjectId end),
		nullId = try(function() return GameObject.NULL_ID end),
		self = { x = vars.playerPosX, y = vars.playerPosY, z = vars.playerPosZ },
		players = try(CoopQA.players),
		horses = try(X.objects, "TppHorse2", X.horseCount or 1),
	}
end
function X.callHorse()
	local pos = try(function() return Vector3(vars.playerPosX + 4, vars.playerPosY + 0.5, vars.playerPosZ + 4) end)
	local r = { horseType = try(function() return BuddyType.HORSE end), before = try(function() return vars.buddyType end) }
	r.obtained = try(function() return TppBuddyService.DidObtainBuddyType(BuddyType.HORSE) end)
	r.canSortie = try(function() return TppBuddyService.CanSortieBuddyType(BuddyType.HORSE) end)
	r.call = try(function() return TppBuddy2BlockController.CallBuddy(BuddyType.HORSE, pos, 0) end)
	r.after = try(function() return vars.buddyType end)
	return r
end
return "ok"
"""

API_TABLES = ["BuddyType", "EntryBuddyType", "TppBuddyService", "TppBuddy2BlockController", "GameObject", "Vehicle",
              "TppHorse2", "Player", "TppNetworkUtil", "Dynamite"]


def run(host, client, log, out):
    sides = (("host", host), ("client", client))
    for name, c in sides:
        out(name + ".lib", c.bridge.exec(LUA_LIB, timeout_ms=5000))
        for t in API_TABLES:
            out("%s.keys.%s" % (name, t), c.eval("CoopQA.x.keys('%s')" % t))
        # one index per request, written out before the next: if an index the game does not have
        # crashes it instead of raising, the file shows which one
        count = 0
        for i in range(4):
            log(name, "look up TppHorse2 index", i)
            one = c.eval("CoopQA.x.objects('TppHorse2', %d)[%d]" % (i + 1, i + 1))
            out("%s.horse.index%d" % (name, i), one)
            if not isinstance(one.get("id"), (int, float)):
                break
            count = i + 1
        c.eval("(function() CoopQA.x.horseCount = %d return %d end)()" % (max(count, 1), count))
        out(name + ".snap.before", c.eval("CoopQA.x.snap()"))

    def sample(label, seconds):
        end = time.time() + seconds
        while time.time() < end:
            for name, c in sides:
                c.must_live()
                out("%s.snap.%s" % (name, label), c.eval("CoopQA.x.snap()"))
            time.sleep(1)

    sample("idle", 5)
    log("host calls D-Horse")
    out("host.callHorse", host.eval("CoopQA.x.callHorse()"))
    sample("after-host-call", 25)
    log("client calls D-Horse")
    out("client.callHorse", client.eval("CoopQA.x.callHorse()"))
    sample("after-client-call", 25)
