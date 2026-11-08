-- Host test for lua/coopqa.lua with stubbed game APIs (Lua 5.1, as in MGSV).
-- Run: lua5.1 qa/tests/test_coopqa.lua <path to qa/lua/>   (prints one JSON reply per line, then OK/FAILED)
package.path = arg[1] .. "/?.lua;" .. package.path

local pumped = 0
local pumpFn = nil
dofile(arg[1] .. "/../tests/stubs.lua")
Dynamite.QAPump = function(f)
	pumped = pumped + 1
	pumpFn = f
end

local qa = require("coopqa")
local failures = 0
local function check(cond, what)
	if not cond then
		failures = failures + 1
		print("FAIL " .. what)
	end
end

qa.Update()
qa.Update()
check(pumped == 2 and pumpFn == qa.dispatch, "Update pumps once per frame with the dispatcher")
check(CoopQA == qa, "module is reachable as CoopQA")

local function reply(kind, arg)
	local r = qa.dispatch(kind, arg)
	check(type(r) == "string", "dispatch returns a string")
	print(r)
	return r
end

check(string.sub(reply("state", ""), 1, 1) == "1", "state ok")
check(not string.find(reply("state", ""), '"players"'), "state without sections has no players (unsafe outside missions)")
check(string.sub(reply("state", "players"), 1, 1) == "0", "players refused in helicopter space")
vars.missionCode = 1
check(string.sub(reply("eval", "CoopQA.players()"), 1, 1) == "0", "players refused on the title screen")
vars.missionCode = 10020
check(string.find(reply("state", "players"), '"players":%[') ~= nil, "players inside a mission")
vars.missionCode = 40010
check(string.sub(reply("eval", "1+2"), 1, 1) == "1", "eval ok")
check(reply("eval", "1+2") == "13", "eval value")
check(reply("eval", "'a\"b\\n'") == '1"a\\"b\\n"', "string escaping")
check(reply("eval", "{1,2,{x=3}}") == '1[1,2,{"x":3}]', "nested tables")
check(reply("eval", "0/0") == "1null", "nan is null")
check(reply("exec", "return 1, 'two'") == '1[1,"two"]', "exec returns all values")
check(reply("exec", "local x = 1") == "1[]", "exec without returns")
check(string.sub(reply("exec", "error('nope')"), 1, 1) == "0", "runtime error is reported, not raised")
check(string.sub(reply("exec", "this is not lua"), 1, 7) == "0compil", "compile error is reported")
check(string.sub(reply("bogus", ""), 1, 1) == "0", "unknown kind")
check(reply("eval", "CoopQA.startMission(10020).ok") == "1true", "startMission from heli")
local reserved = QA_STUB.reserved
check(reserved and reserved.nextMissionId == 10020 and reserved.missionClearType == 7 and reserved.nextClusterId == 3, "ReserveMissionClear args")
vars.missionCode = 10020
check(reply("eval", "CoopQA.startMission(10020).ok") == "1false", "startMission refuses outside heli")
check(reply("eval", "CoopQA.connect().ok") == "1false", "host does not connect")
check(reply("eval", "#CoopQA.players()") == "12", "two players")

print(failures == 0 and "OK" or ("FAILED " .. failures))
