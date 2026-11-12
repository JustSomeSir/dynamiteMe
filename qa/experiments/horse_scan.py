"""Where does the game keep D-Horse's position, and does the other side have a horse there too?

Uses Dynamite.QAScan (hook/src/MemScanCore.h): from the TppHorse2 object, follow pointers and report float
triples near a point. Steps, each written out before the next:
  1. baseline scans on both sides around two points: A (6 m from the host) and B (6 m the other way)
  2. the HOST calls D-Horse to A; after 12 s scan both sides around A and B, three times, 3 s apart
  3. the CLIENT calls D-Horse to B; after 12 s the same scans
New hits compared with the baseline are the horse. Object ids 6656 and 6657 = TppHorse2 index 0 and 1.
Observation only: nothing is asserted.
"""

import time

LUA_LIB = r"""
CoopQA.x = {}
local X = CoopQA.x
local function try(f)
	local ok, r = pcall(f)
	if ok then
		return r
	end
	return "<err " .. tostring(r) .. ">"
end
function X.me()
	return { x = vars.playerPosX, y = vars.playerPosY, z = vars.playerPosZ,
		vehicleId = try(function() return vars.playerVehicleGameObjectId end),
		buddyType = try(function() return vars.buddyType end),
		activeBuddy = try(function() return TppBuddy2BlockController.GetActiveBuddyType() end),
		players = try(CoopQA.players) }
end
function X.scan(id, x, y, z, radius)
	return try(function() return Dynamite.QAScan(id, x, y, z, radius, 4, 2048, 20000) end)
end
function X.call(x, y, z)
	return try(function() return TppBuddy2BlockController.CallBuddy(BuddyType.HORSE, Vector3(x, y, z), 0) end)
end
return "ok"
"""

RADIUS = 5.0


def run(host, client, log, out):
    sides = (("host", host), ("client", client))
    for name, c in sides:
        out(name + ".lib", c.bridge.exec(LUA_LIB, timeout_ms=5000))
    h = host.eval("CoopQA.x.me()")
    out("host.me", h)
    out("client.me", client.eval("CoopQA.x.me()"))
    points = {"A": (h["x"] + 6.0, h["y"], h["z"] + 6.0), "B": (h["x"] - 6.0, h["y"], h["z"] - 6.0),
              # where the exp/two-horses data puts the second TppHorse2 locator (mission 10036 ground start)
              "L": (858.0, 330.5, 1690.0)}
    out("points", points)

    def scans(label):
        for name, c in sides:
            c.must_live()
            for pname, (x, y, z) in points.items():
                for oid in (6656, 6657):
                    log("scan", label, name, pname, oid)
                    text = c.eval("CoopQA.x.scan(%d, %.3f, %.3f, %.3f, %.1f)" % (oid, x, y, z, RADIUS), timeout_ms=30000)
                    out("%s.scan.%s.%s.%d" % (name, label, pname, oid), text)
            out("%s.me.%s" % (name, label), c.eval("CoopQA.x.me()"))

    scans("baseline")
    for who, c, pname in (("host", host, "A"), ("client", client, "B")):
        log(who, "calls D-Horse to point", pname)
        out(who + ".call", c.eval("CoopQA.x.call(%.3f, %.3f, %.3f)" % points[pname]))
        time.sleep(12)
        for i in range(3):
            scans("after-%s-call-%d" % (who, i))
            time.sleep(3)
