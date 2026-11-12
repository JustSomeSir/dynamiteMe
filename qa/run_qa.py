"""MGSV co-op QA runner: starts host + client test copies, drives both through the in-game bridge,
asserts on shared state and prints one last line: RESULT: PASS or RESULT: FAIL.

Runs on the owner's Windows PC (Python 3.8+, standard library only) next to:
    qa/run_qa.py, qa/bridge_client.py (coop-anything tools/bridge), qa/qa_config.json
Usage:
    python run_qa.py                      # every test in the suite
    python run_qa.py --test connect       # one test
    python run_qa.py --twins              # also run the mutation twins (each must FAIL)
    python run_qa.py --keep               # leave the games running at the end

What it touches: only the two test copies listed in qa_config.json and files under qa/runs/.
It kills only processes it started itself. Never the owner's own game, Steam, or saves.
"""

import argparse
import json
import math
import os
import shutil
import socket
import subprocess
import sys
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from bridge_client import Bridge, BridgeError, endpoint_from_file  # noqa: E402


class TestFailure(Exception):
    pass


# ------------------------------------------------------------------ helpers
def now():
    return time.strftime("%H:%M:%S")


class Log:
    def __init__(self, path):
        self.f = open(path, "a", encoding="utf-8")

    def __call__(self, *parts):
        line = "%s %s" % (now(), " ".join(str(p) for p in parts))
        print(line, flush=True)
        self.f.write(line + "\n")
        self.f.flush()


def port_open(port, host="127.0.0.1"):
    s = socket.socket()
    s.settimeout(0.5)
    try:
        s.connect((host, port))
        return True
    except OSError:
        return False
    finally:
        s.close()


def dist(a, b):
    return math.sqrt(sum((a[k] - b[k]) ** 2 for k in ("x", "y", "z")))


class LogTail:
    """Reads only what a game log gained since this object was made (logs are reused across runs)."""

    def __init__(self, path):
        self.path = path
        self.pos = os.path.getsize(path) if os.path.exists(path) else 0
        self.text = ""

    def poll(self):
        if not os.path.exists(self.path):
            return self.text
        size = os.path.getsize(self.path)
        if size < self.pos:  # truncated: the game restarted the log
            self.pos = 0
        with open(self.path, "rb") as f:
            f.seek(self.pos)
            chunk = f.read()
        self.pos += len(chunk)
        self.text += chunk.decode("utf-8", "replace")
        return self.text


# ------------------------------------------------------------------ one game copy
class Copy:
    def __init__(self, name, cfg, log, run_dir):
        self.name = name
        self.dir = cfg["dir"]
        self.exe = cfg.get("exe", os.path.join(self.dir, "mgsvtpp.exe"))
        self.args = cfg.get("args", [])
        self.env_extra = cfg.get("env", {})
        self.log = log
        self.run_dir = run_dir
        self.proc = None
        self.bridge = None
        self.endpoint_file = os.path.join(self.dir, "dynamite", "qa-endpoint.json")
        self.dynlog = LogTail(os.path.join(self.dir, "dynamite", "dynamite.log.txt"))
        self.states = []

    def preflight(self, needs):
        missing = [n for n in needs if not os.path.exists(os.path.join(self.dir, n))]
        if missing:
            raise TestFailure("%s: missing %s" % (self.name, ", ".join(missing)))

    def start(self):
        if os.path.exists(self.endpoint_file):
            os.remove(self.endpoint_file)  # stale from a previous run
        # The game truncates its log at start, but a poll before that would read the previous
        # run's lines (e.g. an old "host session res: 1"). Remove it and read from the start.
        if os.path.exists(self.dynlog.path):
            os.remove(self.dynlog.path)
        self.dynlog.pos = 0
        self.dynlog.text = ""
        env = dict(os.environ)
        env.update(self.env_extra)
        self.log(self.name, "start", self.exe)
        self.proc = subprocess.Popen([self.exe] + self.args, cwd=self.dir, env=env)

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def must_live(self):
        if not self.alive():
            code = self.proc.poll() if self.proc else None
            raise TestFailure("%s exited (code %s): crash or closed" % (self.name, code))

    def connect_bridge(self, timeout_s):
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            self.must_live()
            if os.path.exists(self.endpoint_file):
                try:
                    ep = endpoint_from_file(self.endpoint_file)
                    self.bridge = Bridge.connect(ep, wait_s=10, timeout_s=20)
                    v = self.bridge.version()
                    self.log(self.name, "bridge up:", v.get("game"), v.get("build"))
                    return
                except (BridgeError, OSError, ValueError) as e:
                    self.log(self.name, "bridge not ready yet:", e)
            time.sleep(1)
        raise TestFailure("%s: no bridge after %ds (coopqa.ini missing? hook not loaded?)" % (self.name, timeout_s))

    def state(self, players=False):
        """players=True only inside a mission."""
        self.must_live()
        # short per-request timeout: during loads the game runs no Lua frames and can't answer
        params = {"timeoutMs": 5000}
        if players:
            params["sections"] = ["players"]
        st = self.bridge.call("state", params, timeout_s=10)
        st["_t"] = time.time()
        self.states.append(st)
        return st

    def eval(self, expr, timeout_ms=10000):
        self.must_live()
        return self.bridge.eval(expr, timeout_ms=timeout_ms)

    def wait_state(self, what, pred, timeout_s, poll_s=1.0, prompt=None, prompt_after_s=20):
        """Polls state() until pred(state) is true. Requests can time out while the game loads."""
        deadline = time.time() + timeout_s
        prompted = False
        last = None
        t0 = time.time()
        while time.time() < deadline:
            self.must_live()
            try:
                last = self.state()
                if pred(last):
                    return last
            except BridgeError as e:
                last = {"error": str(e)}
            if prompt and not prompted and time.time() - t0 > prompt_after_s:
                self.log("*** ACTION NEEDED:", prompt)
                prompted = True
            time.sleep(poll_s)
        raise TestFailure("%s: timeout waiting for %s (last state: %s)" % (self.name, what, json.dumps(last)[:400]))

    def wait_log(self, what, needle, timeout_s, fail_needles=()):
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            self.must_live()
            text = self.dynlog.poll()
            if needle in text:
                return True
            for bad in fail_needles:
                if bad in text:
                    return False
            time.sleep(0.5)
        raise TestFailure("%s: timeout waiting for log '%s' (%s)" % (self.name, needle, what))

    def stop(self, keep):
        if self.bridge:
            try:
                self.bridge.close()
            except Exception:
                pass
        if self.proc and self.proc.poll() is None and not keep:
            self.log(self.name, "stopping pid", self.proc.pid)
            self.proc.terminate()
            try:
                self.proc.wait(15)
            except subprocess.TimeoutExpired:
                self.proc.kill()

    def collect(self):
        d = os.path.join(self.run_dir, self.name)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "states.jsonl"), "w", encoding="utf-8") as f:
            for s in self.states:
                f.write(json.dumps(s) + "\n")
        with open(os.path.join(d, "dynamite.log.part.txt"), "w", encoding="utf-8") as f:
            f.write(self.dynlog.poll())
        # newest proxy log of this copy
        logs = sorted((p for p in os.listdir(self.dir) if p.startswith("steamproxy-") and p.endswith(".log")),
                      key=lambda p: os.path.getmtime(os.path.join(self.dir, p)))
        if logs:
            shutil.copy2(os.path.join(self.dir, logs[-1]), os.path.join(d, logs[-1]))


# ------------------------------------------------------------------ the session every test needs
def in_game(st):
    """Past the title and playable. The title runs over the loaded mission, which already reports
    mission 40010 and heliSpace=true; the sequence and the title flag tell the two apart
    (qa/probe_title.py, 1.0.15.4, 2026-11-12)."""
    return (st.get("heliSpace") is True and st.get("sequence") == "Seq_Game_MainGame"
            and st.get("titleMode") is False and st.get("notInGame") is False)


def wait_title(cfg, copy, heli, window):
    """Gets a copy from the title screen into the helicopter.

    title_wait "auto" (default): when the title menu is up, CoopQA.pressContinue() does what selecting
    CONTINUE does. If the game sits in another title step for long, a person is asked to press the key
    it wants, and the step is logged so it can be automated next.
    title_wait "enter": a person presses CONTINUE and then Enter here; nothing is sent before that."""
    t = cfg["timeouts"]
    if cfg.get("title_wait", "auto") == "enter":
        copy.log("*** ACTION NEEDED: press CONTINUE in the %s, wait until you control Snake in the "
                 "helicopter, then press Enter here" % window)
        input()
        copy.must_live()
        copy.wait_state("helicopter", in_game, t["mission_load"])
        copy.log(copy.name, "in helicopter")
        return

    deadline = time.time() + t["title"]
    seq, seq_since, presses, last_press, nagged, last = None, time.time(), 0, 0.0, None, None
    while time.time() < deadline:
        copy.must_live()
        try:
            last = copy.state()
        except BridgeError as e:  # loading: no Lua frames
            last = {"error": str(e)}
            time.sleep(1)
            continue
        if in_game(last):
            copy.log(copy.name, "in helicopter (CONTINUE pressed by the runner %d time(s))" % presses)
            return
        cur = "%s/%s" % (last.get("mission"), last.get("sequence"))
        if cur != seq:
            seq, seq_since = cur, time.time()
            copy.log(copy.name, "title step:", cur)
        waited = time.time() - seq_since
        if last.get("sequence") == "Seq_Game_TitleMenu":
            if waited >= 2 and presses < 3 and time.time() - last_press >= 15:
                r = copy.eval("CoopQA.pressContinue()")
                presses, last_press = presses + 1, time.time()
                copy.log(copy.name, "press CONTINUE:", json.dumps(r))
        elif waited >= 30 and nagged != cur:
            nagged = cur
            copy.log("*** ACTION NEEDED: the %s has been in title step %s for %d s; if it asks for a key, "
                     "press it (this step is not automated yet)" % (window, cur, waited))
        time.sleep(1)
    raise TestFailure("%s: still on the title after %d s (step %s, CONTINUE pressed %d time(s), last state: %s)"
                      % (copy.name, t["title"], seq, presses, json.dumps(last)[:300]))


def start_master_server(cfg, log, run_dir, started):
    """Starts the local master server unless its port is already open."""
    t = cfg["timeouts"]
    if cfg.get("master_server"):
        ms = cfg["master_server"]
        if port_open(ms.get("port", 6667)):
            log("master server already listening on", ms.get("port", 6667))
        else:
            log("start master server:", ms["exe"], " ".join(ms.get("args", [])))
            ms_log = open(os.path.join(run_dir, "master-server.log"), "w")
            server_proc = subprocess.Popen([ms["exe"]] + ms.get("args", []), cwd=ms["dir"],
                                           stdout=ms_log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
            started["server"] = server_proc
            deadline = time.time() + t["master_server"]
            while not port_open(ms.get("port", 6667)):
                if server_proc.poll() is not None:
                    raise TestFailure("master server exited (code %s)" % server_proc.poll())
                if time.time() > deadline:
                    raise TestFailure("master server did not open port %s" % ms.get("port", 6667))
                time.sleep(0.5)
            log("master server up")


def bring_up_session(cfg, log, run_dir, started, skip_connect=False):
    """Master server, host in mission, client connected and in the same mission.

    Everything it starts goes into `started` at once, so the caller can stop it even when this raises."""
    t = cfg["timeouts"]
    start_master_server(cfg, log, run_dir, started)

    host = Copy("host", cfg["host"], log, run_dir)
    client = Copy("client", cfg["client"], log, run_dir)
    started["host"], started["client"] = host, client
    for c in (host, client):
        c.preflight(cfg.get("preflight_files", []))

    mission = cfg["mission"]
    heli = lambda s: s.get("heliSpace") is True  # noqa: E731

    host.start()
    host.connect_bridge(t["bridge"])
    wait_title(cfg, host, heli, "HOST window (game copy 1)")
    log("host in helicopter, starting mission", mission)
    r = host.eval("CoopQA.startMission(%d)" % mission)
    if not r.get("ok"):
        raise TestFailure("host startMission refused: %s" % r)
    host.wait_state("host in mission %d" % mission, lambda s: s.get("mission") == mission, t["mission_load"])
    if not host.wait_log("host session", "host session res: 1", t["mission_load"]):
        raise TestFailure("host session not created")
    log("host session up; settling", t["settle"], "s")
    time.sleep(t["settle"])

    client.start()
    client.connect_bridge(t["bridge"])
    wait_title(cfg, client, heli, "CLIENT window (game copy 2)")
    # Assert the setup before testing anything: copies sharing one save load each other's progress
    # (2026-11-09: the client came up in the host's mission instead of the helicopter).
    hs, cs = host.state().get("saveFile"), client.state().get("saveFile")
    log("save files: host", hs, "client", cs)
    if not hs or hs == cs:
        raise TestFailure("host and client use the same save file %r; give the client its own (qa/instance_saves.py)" % hs)
    if skip_connect:
        log("TWIN: client does NOT call CreateClientSession")
    else:
        for attempt in range(1, 4):
            log("client connect, attempt", attempt)
            client.eval("CoopQA.connect()")
            ok = client.wait_log("connection", "Co-op connection established", t["connect"],
                                 fail_needles=("dynamite_connection_failed",))
            if ok:
                break
            client.dynlog.text = ""  # forget the failure line before the next try
            time.sleep(5)
        else:
            raise TestFailure("client could not connect after 3 attempts")
        log("client connected")
    r = client.eval("CoopQA.startMission(%d)" % mission)
    if not r.get("ok"):
        raise TestFailure("client startMission refused: %s" % r)
    client.wait_state("client in mission %d" % mission, lambda s: s.get("mission") == mission, t["mission_load"])
    log("client in mission; settling", t["settle"], "s")
    time.sleep(t["settle"])
    return host, client


# ------------------------------------------------------------------ tests: each returns a list of failed assertions
def assert_mode(host, client, log):
    """Both copies agree they are in one 2-player co-op session in the same mission (guards hollow passes)."""
    fails = []
    hs, cs = host.state(), client.state()
    log("host state:", json.dumps({k: hs.get(k) for k in ("mission", "isHost", "isClient", "members", "sessionConnect")}))
    log("client state:", json.dumps({k: cs.get(k) for k in ("mission", "isHost", "isClient", "members", "sessionConnect")}))
    if hs.get("isHost") is not True:
        fails.append("host copy does not report isHost")
    if cs.get("isClient") is not True:
        fails.append("client copy does not report isClient")
    if hs.get("members") != 2:
        fails.append("host sees %s members, expected 2" % hs.get("members"))
    if cs.get("members") != 2:
        fails.append("client sees %s members, expected 2" % cs.get("members"))
    if hs.get("mission") != cs.get("mission"):
        fails.append("missions differ: host %s, client %s" % (hs.get("mission"), cs.get("mission")))
    return fails


def assert_positions(host, client, log, tolerance_m, partner_index_shift=0):
    """Each side sees both players where the other side says they are. Both players stand still."""
    fails = []
    hs, cs = host.state(players=True), client.state(players=True)
    hp = {p["index"]: p for p in hs.get("players", [])}
    cp = {p["index"]: p for p in cs.get("players", [])}
    if len(hp) < 2 or len(cp) < 2:
        return ["players missing: host sees %d, client sees %d" % (len(hp), len(cp))]
    for i in (0, 1):
        j = (i + partner_index_shift) % 2
        d = dist(hp[i], cp[j])
        log("player %d: host view %s, client view %s, distance %.2f m" % (
            i, {k: round(hp[i][k], 2) for k in "xyz"}, {k: round(cp[j][k], 2) for k in "xyz"}, d))
        if d > tolerance_m:
            fails.append("player %d differs by %.2f m between copies (tolerance %.1f)" % (i, d, tolerance_m))
    if dist(hp[0], hp[1]) < 0.01:
        fails.append("players 0 and 1 are at the same spot: positions look unset")
    return fails


def assert_stability(host, client, log, soak_s):
    """For soak_s seconds both copies stay alive and keep a 2-player session (no crash, no drop)."""
    fails = []
    t_end = time.time() + soak_s
    samples = 0
    while time.time() < t_end:
        for c in (host, client):
            if not c.alive():
                return ["%s exited (code %s) during the soak" % (c.name, c.proc.poll())]
            try:
                st = c.state()
            except BridgeError as e:
                return ["%s stopped answering during the soak: %s" % (c.name, e)]
            if st.get("members") != 2:
                fails.append("%s dropped to %s members after %.0f s" % (c.name, st.get("members"), soak_s - (t_end - time.time())))
                return fails
        samples += 1
        time.sleep(1)
    log("stability: %d samples over %d s, both alive, 2 members throughout" % (samples, soak_s))
    return fails


TESTS = {
    "connect": lambda h, c, log, cfg: assert_mode(h, c, log),
    "stability": lambda h, c, log, cfg: assert_stability(h, c, log, cfg["timeouts"]["soak"]),
    "positions": lambda h, c, log, cfg: assert_positions(h, c, log, cfg["position_tolerance_m"]),
}

# Oracle twins run in the same session with a deliberately wrong expectation; each MUST fail.
ORACLE_TWINS = {
    "positions": lambda h, c, log, cfg: assert_positions(h, c, log, cfg["position_tolerance_m"], partner_index_shift=1),
}


def run_session(label, tests, cfg, args, runs_root, skip_connect=False):
    """Brings up one host+client session, runs `tests` and their oracle twins. Returns [(name, kind, ok)].

    kind is "test" (ok = passed) or "twin" (ok = the twin failed, as it must)."""
    run_dir = os.path.join(runs_root, time.strftime("%Y%m%d-%H%M%S") + "-" + label)
    os.makedirs(run_dir, exist_ok=True)
    log = Log(os.path.join(run_dir, "runner.log"))
    log("=== session %s: tests %s%s" % (label, ", ".join(tests), " (client does not connect)" if skip_connect else ""))
    started = {}
    results = []
    session_error = None
    try:
        host, client = bring_up_session(cfg, log, run_dir, started, skip_connect=skip_connect)
        for name in tests:
            try:
                fails = TESTS[name](host, client, log, cfg)
            except (TestFailure, BridgeError) as e:
                fails = [str(e)]
            for f in fails:
                log("ASSERT FAILED [%s]: %s" % (name, f))
            log("--- %s: %s" % (name, "PASS" if not fails else "FAIL"))
            results.append((name, "test", not fails, fails))
            if name in ORACLE_TWINS and args.twins:
                try:
                    tf = ORACLE_TWINS[name](host, client, log, cfg)
                except (TestFailure, BridgeError) as e:
                    tf = [str(e)]
                log("--- %s oracle twin: %s" % (name, "failed as it must" if tf else "PASSED -> test is hollow"))
                results.append((name, "twin", bool(tf), tf))
    except TestFailure as e:
        session_error = str(e)
    except Exception as e:  # never a pass
        session_error = "runner error: %r" % (e,)
        log(traceback.format_exc())
    finally:
        for key in ("client", "host"):
            c = started.get(key)
            if c:
                try:
                    c.collect()
                except Exception as e:
                    log("collect", c.name, "failed:", e)
                c.stop(args.keep)
        server = started.get("server")
        if server and not args.keep:
            server.terminate()
    if session_error:
        log("SESSION FAILED:", session_error)
        done = {n for n, _, _, _ in results}
        results += [(n, "test", False, [session_error]) for n in tests if n not in done]
    with open(os.path.join(run_dir, "result.json"), "w", encoding="utf-8") as f:
        json.dump([{"test": n, "kind": k, "ok": ok, "fails": fl} for n, k, ok, fl in results] +
                  ([{"session_error": session_error}] if session_error else []), f, indent=1)
    return results, session_error


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default=os.path.join(HERE, "qa_config.json"))
    ap.add_argument("--test", action="append", help="run only these tests")
    ap.add_argument("--twins", action="store_true", help="also run mutation twins (each must FAIL)")
    ap.add_argument("--keep", action="store_true", help="leave games running")
    args = ap.parse_args()
    with open(args.config, encoding="utf-8") as f:
        cfg = json.load(f)
    runs_root = cfg.get("runs_dir", os.path.join(HERE, "runs"))
    names = args.test or cfg.get("suite", ["connect", "positions", "stability"])
    for n in names:
        if n not in TESTS:
            print("unknown test", n)
            return 2

    rows = []
    results, _ = run_session("main", names, cfg, args, runs_root)
    rows += results
    if args.twins and "connect" in names:
        # Twin of the whole session: the client never connects, so `connect` must fail.
        tres, terr = run_session("twin-noconnect", ["connect"], cfg, args, runs_root, skip_connect=True)
        failed_as_it_must = any(not ok for _, _, ok, _ in tres) or terr is not None
        rows.append(("connect", "twin", failed_as_it_must, [] if failed_as_it_must else ["connect passed without CreateClientSession"]))

    print()
    for n, kind, ok, fails in rows:
        print("%-10s %-5s %s%s" % (n, kind, "ok" if ok else "NOT OK", "" if ok else "  <- " + "; ".join(fails)[:200]))
    all_ok = all(ok for _, _, ok, _ in rows)
    print("RESULT: %s" % ("PASS" if all_ok else "FAIL"))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
