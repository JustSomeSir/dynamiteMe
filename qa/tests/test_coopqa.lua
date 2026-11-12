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

-- MGSV loads scripts with their own environment; loadstring chunks get the thread's globals instead.
-- Load coopqa the same way so eval/exec must still see CoopQA (failed in the game on 2026-11-09).
local chunk = assert(loadfile(arg[1] .. "/coopqa.lua"))
local sandbox = setmetatable({}, { __index = _G })
sandbox._G = sandbox
setfenv(chunk, sandbox)
local qa = chunk()
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
check(sandbox.CoopQA == qa and rawget(_G, "CoopQA") == nil, "CoopQA is set in the module environment only")
vars.missionCode = 1
qa.Update()
check(pumped == 3, "pumps on the title screen too (mission 1)")
vars.missionCode = 40010

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
check(reply("eval", "CoopQA.version") == "11", "eval chunks see CoopQA")
check(reply("eval", "'a\"b\\n'") == '1"a\\"b\\n"', "string escaping")
check(reply("eval", "{1,2,{x=3}}") == '1[1,2,{"x":3}]', "nested tables")
check(reply("eval", "0/0") == "1null", "nan is null")
check(reply("eval", "-11.5") == "1-11.500000", "fractions use a decimal point")
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

-- title screen
local pressed = 0
local seqName = "Seq_Demo_StartHasTitleMission"
TppSequence = { GetCurrentSequenceName = function() return seqName end }
gvars = { ini_isTitleMode = true }
mvars = { mis_missionStateIsNotInGame = true, seq_sequenceTable = { Seq_Game_TitleMenu = { OnSelectContinue = function() pressed = pressed + 1 end } } }
vars.missionCode = 40010
check(string.find(reply("state", ""), '"sequence":"Seq_Demo_StartHasTitleMission"') ~= nil, "state reports the sequence")
check(string.find(reply("state", ""), '"titleMode":true') ~= nil, "state reports title mode")
check(reply("eval", "CoopQA.pressContinue().ok") == "1false" and pressed == 0, "no CONTINUE outside the title menu")
seqName = "Seq_Game_TitleMenu"
check(reply("eval", "CoopQA.pressContinue().ok") == "1true" and pressed == 1, "CONTINUE calls the menu's OnSelectContinue")
mvars.seq_sequenceTable.Seq_Game_TitleMenu.OnSelectContinue = nil
check(reply("eval", "CoopQA.pressContinue().ok") == "1false", "missing handler is reported, not raised")

print(failures == 0 and "OK" or ("FAILED " .. failures))
