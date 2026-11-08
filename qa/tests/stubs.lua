-- Stubbed game APIs for host tests of coopqa.lua (shared by test_coopqa.lua and qabridge_host).
Dynamite = {
	IsHost = function()
		return true
	end,
	IsClient = function()
		return false
	end,
	QAMemberCount = function()
		return 2
	end,
	QAPlayerPos = function(i)
		return 10 + i, 20.5, -30
	end,
	IsSynchronized = function()
		error("boom") -- a failing game call must not break state()
	end,
	CreateClientSession = function() end,
}
vars = { missionCode = 40010, playerPosX = 1, playerPosY = 2, playerPosZ = 3, playerLife = 6000 }
QA_STUB = { reserved = nil }
TppDefine = { MISSION_CLEAR_TYPE = { FROM_HELISPACE = 7 }, STORY_MISSION_CLUSTER_ID = { [10020] = 3 } }
TppMission = {
	IsHelicopterSpace = function(c)
		return c == 40010 or c == 40020
	end,
	ReserveMissionClear = function(t)
		QA_STUB.reserved = t
		return true
	end,
}


QA_HOST = { freezeMs = 0, quit = false, stackErrors = 0 }
