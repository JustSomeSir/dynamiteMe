"""Tests run_qa.py end to end against two simulated games (qabridge_host + sim_stubs.lua), no MGSV.

    python3 -I qa/tests/test_runner_sim.py --lua-src <lua 5.1 src dir>

Scenarios (each runs the real runner as a subprocess and checks its RESULT line and exit code):
  good           everything works                         -> RESULT: PASS (with --twins: both twins fail as they must)
  desync         client sees the host 5 m off              -> RESULT: FAIL (positions)
  noconnect      CreateClientSession always fails          -> RESULT: FAIL (could not connect)
  sharedsave     both copies use one save file             -> RESULT: FAIL (same save file)
  titlestuck     CONTINUE does nothing on the host          -> RESULT: FAIL (still on the title)
  crash          client exits during the mission           -> RESULT: FAIL (exited)
Last line: ALL OK only if every scenario gave the expected verdict.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
QA = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from test_qabridge import build  # noqa: E402

REPO = os.path.dirname(QA)


def scenario(exe, name, env_extra, want_pass, expect_text, twins):
    root = tempfile.mkdtemp(prefix="qa-sim-%s-" % name)
    shared = os.path.join(root, "net")
    os.makedirs(shared)
    copies = {}
    for role, d in (("host", "game"), ("client", "game2")):
        os.makedirs(os.path.join(root, d, "dynamite"))
        env = {"QA_SIM_ROLE": role, "QA_SIM_SHARED": shared}
        env.update(env_extra.get(role, {}))
        copies[role] = {
            "dir": os.path.join(root, d),
            "exe": exe,
            "args": [os.path.join(QA, "lua"), os.path.join(HERE, "sim_stubs.lua"), "dynamite/qa-endpoint.json"],
            "env": env,
        }
    cfg = {
        "host": copies["host"],
        "client": copies["client"],
        "master_server": None,
        "preflight_files": [],
        "mission": 10020,
        "position_tolerance_m": 1.0,
        "runs_dir": os.path.join(root, "runs"),
        "suite": ["connect", "positions", "stability"],
        "timeouts": {"master_server": 10, "bridge": 20, "title": 20, "mission_load": 20, "connect": 5, "settle": 1, "soak": 5},
    }
    cfg_path = os.path.join(root, "qa_config.json")
    with open(cfg_path, "w") as f:
        json.dump(cfg, f)
    args = [sys.executable, "-I", os.path.join(QA, "run_qa.py"), "--config", cfg_path]
    if twins:
        args.append("--twins")
    p = subprocess.run(args, capture_output=True, text=True, timeout=600)
    out = p.stdout + p.stderr
    last = [l for l in out.splitlines() if l.startswith("RESULT:")]
    got_pass = bool(last) and last[-1] == "RESULT: PASS"
    ok = (got_pass == want_pass) and (p.returncode == (0 if want_pass else 1)) and (expect_text in out)
    print("%s %-10s want %s, got %s, exit %d%s" % (
        "PASS" if ok else "FAIL", name, "PASS" if want_pass else "FAIL", last[-1] if last else "no RESULT line", p.returncode,
        "" if expect_text in out else "  (missing text %r)" % expect_text))
    if not ok:
        print("\n".join("    | " + l for l in out.splitlines()[-40:]))
    shutil.rmtree(root, ignore_errors=True)
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lua-src", required=True)
    a = ap.parse_args()
    bdir = tempfile.mkdtemp(prefix="qa-sim-build-")
    exe = os.path.join(bdir, "host")
    build(a.lua_src, os.path.join(REPO, "hook", "src"), exe, os.path.join(bdir, "lua"))
    results = [
        scenario(exe, "good", {}, True, "positions  twin  ok", twins=True),
        scenario(exe, "desync", {"client": {"QA_SIM_DESYNC": "1"}}, False, "player 0 differs", twins=False),
        scenario(exe, "noconnect", {"client": {"QA_SIM_NOCONNECT": "1"}}, False, "could not connect", twins=False),
        scenario(exe, "sharedsave", {"client": {"QA_SIM_SHARED_SAVE": "1"}}, False, "same save file", twins=False),
        scenario(exe, "titlestuck", {"host": {"QA_SIM_TITLE_STUCK": "1"}}, False, "still on the title", twins=False),
        scenario(exe, "crash", {"client": {"QA_SIM_CRASH_IN_MISSION": "1"}}, False, "[stability]: client", twins=False),
    ]
    shutil.rmtree(bdir, ignore_errors=True)
    print("ALL OK" if all(results) else "FAILED")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
