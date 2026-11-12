"""Brings up the same host + client session as run_qa.py, then runs one experiment from qa/experiments/.

    python experiment.py horse_probe [--config qa_config.json]

An experiment is a module with run(host, client, log, out): it sends Lua to the two games through the
bridge and writes what it sees with out(tag, data) into runs/<time>-<name>/experiment.jsonl.
Experiments observe and try things; they are not tests and never print PASS.
"""

import argparse
import importlib
import json
import os
import sys
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run_qa  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("--config", default=os.path.join(HERE, "qa_config.json"))
    ap.add_argument("--keep", action="store_true", help="leave games running")
    args = ap.parse_args()
    with open(args.config, encoding="utf-8") as f:
        cfg = json.load(f)
    mod = importlib.import_module("experiments." + args.name)
    run_dir = os.path.join(cfg.get("runs_dir", os.path.join(HERE, "runs")), time.strftime("%Y%m%d-%H%M%S") + "-" + args.name)
    os.makedirs(run_dir, exist_ok=True)
    log = run_qa.Log(os.path.join(run_dir, "runner.log"))
    data = open(os.path.join(run_dir, "experiment.jsonl"), "w", encoding="utf-8")

    def out(tag, value):
        data.write(json.dumps({"t": round(time.time(), 2), "tag": tag, "data": value}, sort_keys=True) + "\n")
        data.flush()

    started = {}
    status = "did not finish"
    try:
        host, client = run_qa.bring_up_session(cfg, log, run_dir, started)
        log("=== experiment", args.name)
        mod.run(host, client, log, out)
        status = "finished"
    except Exception as e:
        status = "stopped: %r" % (e,)
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
        if started.get("server") and not args.keep:
            started["server"].terminate()
    log("EXPERIMENT", args.name, status, "->", run_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
