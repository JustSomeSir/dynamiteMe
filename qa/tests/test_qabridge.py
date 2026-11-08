"""End-to-end test of the QA bridge without the game.

Builds qa/tests/qabridge_host.cpp (real coopbridge server + real Lua 5.1 + hook/src/QABridgeCore.h)
and drives it with coop-anything's tools/bridge/bridge_client.py, the same client the runner uses.
Then builds two broken copies of QABridgeCore.h (mutation twins) and checks that the tests catch them.

    python3 -I qa/tests/test_qabridge.py --lua-src <lua 5.1 src dir> --bridge-client <tools/bridge dir>

Prints PASS/FAIL per check and, last, "ALL OK" only if every check passed and every twin was caught.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))


def build(lua_src, core_dir, out, lua_obj_dir):
    os.makedirs(lua_obj_dir, exist_ok=True)
    objs = []
    for name in sorted(os.listdir(lua_src)):
        if not name.endswith(".c") or name in ("lua.c", "luac.c", "print.c"):
            continue
        obj = os.path.join(lua_obj_dir, name[:-2] + ".o")
        if not os.path.exists(obj):
            subprocess.run(["gcc", "-O1", "-w", "-DLUA_USE_POSIX", "-c", os.path.join(lua_src, name), "-o", obj], check=True)
        objs.append(obj)
    cmd = [
        "g++", "-std=c++20", "-O1", "-g", "-Wall", "-Werror", "-pthread",
        "-I", core_dir, "-I", os.path.join(REPO, "hook", "src"), "-I", lua_src,
        os.path.join(HERE, "qabridge_host.cpp"), *objs, "-o", out, "-lm", "-ldl",
    ]
    subprocess.run(cmd, check=True)


class Host:
    def __init__(self, exe, workdir):
        self.endpoint = os.path.join(workdir, "endpoint.json")
        self.proc = subprocess.Popen(
            [exe, os.path.join(REPO, "qa", "lua"), os.path.join(HERE, "stubs.lua"), self.endpoint],
            stderr=subprocess.PIPE, text=True)

    def stop(self):
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait(5)
        return self.proc.stderr.read()


def run_checks(exe, bridge):
    """Returns (passed, failed) names."""
    from bridge_client import Bridge, CallTimeout, RpcError, endpoint_from_file  # noqa: E402

    passed, failed = [], []

    def check(cond, name):
        (passed if cond else failed).append(name)

    work = tempfile.mkdtemp(prefix="qabridge-")
    host = Host(exe, work)
    try:
        for _ in range(100):
            if os.path.exists(host.endpoint):
                break
            time.sleep(0.05)
        ep = endpoint_from_file(host.endpoint)
        with Bridge.connect(ep, wait_s=10, timeout_s=5) as g:
            v = g.version()
            check(v.get("game") == "qabridge-host", "version over the bridge")
            g.require("exec", "eval", "state")
            check(True, "exec/eval/state advertised")

            st = g.state()
            check(st.get("members") == 2 and st.get("mission") == 40010 and st.get("isHost") is True, "state() from coopqa via Lua")
            check("players" not in st, "no positions unless asked (they hang the title screen)")
            try:
                g.state(["players"])
                check(False, "positions refused in helicopter space")
            except RpcError as e:
                check("only available inside a mission" in str(e), "positions refused in helicopter space")
            g.exec("vars.missionCode = 10020")
            sp = g.state(["players"])
            check(len(sp.get("players", [])) == 2 and sp["players"][1]["x"] == 11, "state players and positions in a mission")
            g.exec("vars.missionCode = 40010")
            check("synchronized" not in st, "failing game call left out of state, not fatal")

            check(g.eval("1+2") == 3, "eval number")
            check(g.eval("{a=1,b={2,3}}") == {"a": 1, "b": [2, 3]}, "eval table")
            out = g.exec("return 'x', 2")
            check(out.get("ok") is True and out.get("output") == '["x",2]', "exec output is JSON of returned values")

            try:
                g.exec("error('kaputt')")
                check(False, "Lua runtime error becomes an RpcError")
            except RpcError as e:
                check(e.code == -32014 and "kaputt" in str(e), "Lua runtime error becomes an RpcError")
            try:
                g.exec("this is not lua")
                check(False, "Lua compile error becomes an RpcError")
            except RpcError as e:
                check(e.code == -32014 and "compile" in str(e), "Lua compile error becomes an RpcError")

            check(g.eval("CoopQA.startMission(10020).ok") is True and g.eval("QA_STUB.reserved.nextMissionId") == 10020,
                  "startMission reaches TppMission.ReserveMissionClear")

            # many calls: the pump must leave the Lua stack untouched every frame
            for i in range(300):
                g.eval(str(i))
            check(g.eval("QA_HOST.stackErrors") == 0, "Lua stack balanced over 300 calls")

            # state and an exec that modifies game state, seen by the next state()
            g.exec("vars.missionCode = 10020")
            check(g.state().get("mission") == 10020, "exec changes are visible to state()")

            # a stuck main thread: the call times out instead of hanging, the next call works
            g.exec("QA_HOST.freezeMs = 4000")
            t0 = time.time()
            try:
                g.eval("1", timeout_ms=500)
                check(False, "stuck main thread -> timeout")
            except (CallTimeout, RpcError):
                check(time.time() - t0 < 3.5, "stuck main thread -> timeout")
            time.sleep(4.0)
            check(g.eval("2") == 2, "bridge recovers after the freeze")

            g.exec("QA_HOST.quit = true")
        time.sleep(0.5)
        check(host.proc.poll() == 0, "host exits cleanly")
    except Exception as e:  # any unexpected error is a failure, never a pass
        failed.append("unexpected: %r" % (e,))
    finally:
        err = host.stop()
        shutil.rmtree(work, ignore_errors=True)
        if failed and err:
            print("  host stderr:", err.strip()[-800:])
    return passed, failed


TWINS = [
    # (name, old, new): each breaks one guard in QABridgeCore.h
    ("stack not restored", "            api_.remove(L_, -1);\n", ""),
    ("Lua error reported as success", "            if (out[0] == '0') {", "            if (false) {"),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lua-src", required=True)
    ap.add_argument("--bridge-client", required=True, help="coop-anything tools/bridge directory")
    a = ap.parse_args()
    sys.path.insert(0, a.bridge_client)

    build_dir = tempfile.mkdtemp(prefix="qabridge-build-")
    lua_objs = os.path.join(build_dir, "lua")
    exe = os.path.join(build_dir, "host")
    build(a.lua_src, os.path.join(REPO, "hook", "src"), exe, lua_objs)
    passed, failed = run_checks(exe, a.bridge_client)
    for n in passed:
        print("PASS", n)
    for n in failed:
        print("FAIL", n)

    caught = 0
    for name, old, new in TWINS:
        core = open(os.path.join(REPO, "hook", "src", "QABridgeCore.h")).read()
        assert old in core, "twin %r does not apply" % name
        tdir = os.path.join(build_dir, "twin_" + str(caught))
        os.makedirs(tdir, exist_ok=True)
        open(os.path.join(tdir, "QABridgeCore.h"), "w").write(core.replace(old, new))
        shutil.copytree(os.path.join(REPO, "hook", "src", "coopbridge"), os.path.join(tdir, "coopbridge"))
        texe = os.path.join(tdir, "host")
        build(a.lua_src, tdir, texe, lua_objs)
        _, tfailed = run_checks(texe, a.bridge_client)
        if tfailed:
            caught += 1
            print("TWIN caught: %s (%d checks failed)" % (name, len(tfailed)))
        else:
            print("TWIN SURVIVED: %s" % name)
    shutil.rmtree(build_dir, ignore_errors=True)

    ok = not failed and caught == len(TWINS)
    print("ALL OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
