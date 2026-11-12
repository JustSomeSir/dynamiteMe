"""Records what the game does while a person goes through the title screen. Read-only: it changes nothing
in the game except wrapping one message handler to listen.

    python probe_title.py [--config qa_config.json]

It starts the master server and the HOST copy, then you go through the title as usual (confirm the
messages, CONTINUE), wait until you control Snake in the helicopter and press Enter in this console.
Output: runs/<time>-probe/ with
  timeline.log   every change of mission, sequence name and title flags, with time
  events.log     game messages (sender hash, message hash, args) seen by the Lua side
  dump-*.txt     the sequence list and the message handlers of each sequence the game was in
  names.json     hash -> name for known message names (qa/message_names.txt)
With these the runner can learn to tell the title from the helicopter and to press CONTINUE itself.
"""

import argparse
import json
import os
import sys
import threading
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run_qa  # noqa: E402
from bridge_client import BridgeError  # noqa: E402

# Installed once. Listens through OnlineChallengeTask.OnMessage: TppMain.OnMessage looks that global up on
# every message, so a wrapper there sees every message without replacing anything the game cached.
LUA_INSTALL = r"""
if not CoopQA.probe then
	local P = { events = {}, n = 0 }
	CoopQA.probe = P
	local function rec(sender, msg, a0, a1, a2, a3)
		P.n = P.n + 1
		local e = P.events
		e[#e + 1] = { n = P.n, frame = CoopQA.frame, sender = sender, msg = msg, a0 = a0, a1 = a1, a2 = a2, a3 = a3 }
		if #e > 600 then
			table.remove(e, 1)
		end
	end
	-- scripts are reloaded between missions, so the wrapper is put back whenever it is missing
	function P.ensureHook()
		local t = OnlineChallengeTask
		if type(t) ~= "table" or type(t.OnMessage) ~= "function" then
			P.hook = "none"
			return
		end
		if P.wrapper ~= t.OnMessage then
			local orig = t.OnMessage
			P.wrapper = function(...)
				pcall(rec, ...)
				return orig(...)
			end
			t.OnMessage = P.wrapper
			P.hooks = (P.hooks or 0) + 1
		end
		P.hook = "OnlineChallengeTask.OnMessage"
	end
	function P.take()
		local e = P.events
		P.events = {}
		return e
	end
	local function get(f)
		local ok, v = pcall(f)
		if ok then
			return v
		end
		return "<err>"
	end
	function P.snap()
		pcall(P.ensureHook)
		return {
			hook = P.hook,
			hooks = P.hooks,
			mission = get(function() return vars.missionCode end),
			seq = get(function() return TppSequence.GetCurrentSequenceName() end),
			title = get(function() return gvars.ini_isTitleMode end),
			returnToTitle = get(function() return gvars.ini_isReturnToTitle end),
			heli = get(function() return TppMission.IsHelicopterSpace(vars.missionCode) end),
			notInGame = get(function() return mvars.mis_missionStateIsNotInGame end),
			disableAction = get(function() return vars.playerDisableActionFlag end),
			padMask = get(function() return TppGameStatus.IsSet("TppMain.lua", "S_DISABLE_PLAYER_PAD") end),
		}
	end
	-- text dump of a table tree: cycles and size are bounded
	function P.dump(root, name, maxDepth, maxLines)
		local out, seen = {}, {}
		local function walk(v, path, depth)
			if #out >= maxLines then
				return
			end
			local t = type(v)
			if t ~= "table" then
				out[#out + 1] = path .. " = " .. t .. " " .. tostring(v)
				return
			end
			if seen[v] then
				out[#out + 1] = path .. " = <seen " .. seen[v] .. ">"
				return
			end
			seen[v] = path
			if depth >= maxDepth then
				out[#out + 1] = path .. " = <table>"
				return
			end
			local keys = {}
			for k in pairs(v) do
				keys[#keys + 1] = k
			end
			table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
			if #keys == 0 then
				out[#out + 1] = path .. " = {}"
			end
			for _, k in ipairs(keys) do
				walk(v[k], path .. "." .. tostring(k), depth + 1)
			end
		end
		walk(root, name, 0)
		return out
	end
	function P.describe()
		local out = {}
		local function add(lines)
			for _, l in ipairs(lines) do
				out[#out + 1] = l
			end
		end
		out[#out + 1] = "current sequence: " .. tostring(get(function() return TppSequence.GetCurrentSequenceName() end))
		add(P.dump(get(function() return mvars.seq_sequenceNames end), "mvars.seq_sequenceNames", 2, 200))
		local cur = get(function() return mvars.seq_sequenceTable[mvars.seq_currentSequence] end)
		add(P.dump(cur, "currentSequenceTable", 7, 600))
		-- every global whose name mentions the title, and title-ish members of global tables
		for _, envName in ipairs({ "thread", "module" }) do
			local G = envName == "thread" and getfenv(0) or _G
			if type(G) == "table" then
				for k, v in pairs(G) do
					if type(k) == "string" then
						if string.find(string.lower(k), "title") then
							add(P.dump(v, envName .. ":" .. k, 3, 300))
						elseif type(v) == "table" then
							for k2, v2 in pairs(v) do
								if type(k2) == "string" and (string.find(k2, "Title") or string.find(k2, "Continue") or string.find(k2, "PressStart")) then
									out[#out + 1] = envName .. ":" .. k .. "." .. k2 .. " = " .. type(v2)
								end
							end
						end
					end
				end
			end
		end
		return out
	end
end
pcall(CoopQA.probe.ensureHook)
return CoopQA.probe.hook
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default=os.path.join(HERE, "qa_config.json"))
    ap.add_argument("--max-minutes", type=float, default=15)
    args = ap.parse_args()
    with open(args.config, encoding="utf-8") as f:
        cfg = json.load(f)
    run_dir = os.path.join(cfg.get("runs_dir", os.path.join(HERE, "runs")), time.strftime("%Y%m%d-%H%M%S") + "-probe")
    os.makedirs(run_dir, exist_ok=True)
    log = run_qa.Log(os.path.join(run_dir, "runner.log"))
    timeline = open(os.path.join(run_dir, "timeline.log"), "w", encoding="utf-8")
    events = open(os.path.join(run_dir, "events.log"), "w", encoding="utf-8")
    started = {}
    done = threading.Event()

    def note(text):
        timeline.write("%s %s\n" % (time.strftime("%H:%M:%S"), text))
        timeline.flush()

    try:
        run_qa.start_master_server(cfg, log, run_dir, started)
        host = run_qa.Copy("host", cfg["host"], log, run_dir)
        started["host"] = host
        host.start()
        host.connect_bridge(cfg["timeouts"]["bridge"])
        log("*** Go through the title as usual (messages, CONTINUE). When you control Snake in the helicopter, "
            "press Enter here.")
        threading.Thread(target=lambda: (input(), done.set()), daemon=True).start()

        installed = False
        last_snap = None
        answering = None
        dumped = set()
        deadline = time.time() + args.max_minutes * 60
        while not done.is_set() and time.time() < deadline and host.alive():
            try:
                if not installed:
                    hook = host.bridge.exec(LUA_INSTALL, timeout_ms=2000)
                    note("probe installed: %s" % json.dumps(hook))
                    installed = True
                    names = open(os.path.join(HERE, "message_names.txt"), encoding="utf-8").read().split()
                    table = {}
                    for i in range(0, len(names), 60):
                        chunk = names[i:i + 60]
                        expr = "{" + ",".join("Fox.StrCode32('%s')" % n for n in chunk) + "}"
                        for n, h in zip(chunk, host.bridge.eval(expr, timeout_ms=2000)):
                            table[str(h)] = n
                    with open(os.path.join(run_dir, "names.json"), "w", encoding="utf-8") as f:
                        json.dump(table, f, indent=0, sort_keys=True)
                snap = host.bridge.eval("CoopQA.probe.snap()", timeout_ms=2000)
                if answering is not True:
                    note("game answers")
                    answering = True
                if snap != last_snap:
                    note("state " + json.dumps(snap, sort_keys=True))
                    last_snap = snap
                key = "%s-%s" % (snap.get("mission"), snap.get("seq"))
                if key not in dumped:
                    dumped.add(key)
                    lines = host.bridge.eval("CoopQA.probe.describe()", timeout_ms=5000)
                    safe = "".join(c if c.isalnum() or c in "_.-" else "_" for c in key)
                    with open(os.path.join(run_dir, "dump-%02d-%s.txt" % (len(dumped), safe)), "w", encoding="utf-8") as f:
                        f.write("\n".join(lines) + "\n")
                    note("dumped %s (%d lines)" % (key, len(lines)))
                for e in host.bridge.eval("CoopQA.probe.take()", timeout_ms=2000):
                    events.write("%s %s\n" % (time.strftime("%H:%M:%S"), json.dumps(e, sort_keys=True)))
                events.flush()
            except BridgeError as e:
                # during loads the game runs no Lua frames; a reinstall is needed if the Lua state was rebuilt
                if answering is not False:
                    note("no answer: %s" % str(e)[:200])
                    answering = False
                if "probe" in str(e) and "nil" in str(e):
                    installed = False
            time.sleep(0.5)
        note("end: %s" % ("Enter pressed" if done.is_set() else ("game closed" if not host.alive() else "time limit")))
    except Exception:
        log(traceback.format_exc())
    finally:
        h = started.get("host")
        if h:
            try:
                h.collect()
            except Exception as e:
                log("collect failed:", e)
            h.stop(False)
        if started.get("server"):
            started["server"].terminate()
    print("probe output:", run_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
