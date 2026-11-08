"""Tests qa/instance_saves.py on a fake Steam account folder (no real saves).

    python3 -I qa/tests/test_instance_saves.py
The codec itself was compared byte for byte with squib's Go save.Decode (2026-11-09, 103-byte input).
"""

import hashlib
import io
import os
import shutil
import sys
import tempfile
import zipfile
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import instance_saves as s  # noqa: E402

fails = []


def check(cond, name):
    print(("PASS " if cond else "FAIL ") + name)
    if not cond:
        fails.append(name)


def fake_save(key, size):
    body = bytes((i * 31 + len(key)) & 0xFF for i in range(size))
    return s.xor_stream(key, hashlib.md5(body).digest() + body)


def make_account(root):
    remote = os.path.join(root, "311340", "remote")
    local = os.path.join(root, "287700", "local")
    os.makedirs(remote)
    os.makedirs(local)
    files = {}
    for i, n in enumerate(s.NAMES):
        key = "DYNAMITE_" + n
        data = fake_save(key, 1000 + i * 37)  # odd sizes: the tail bytes path
        for d, name in ((remote, key), (local, key + "0"), (local, key + "1")):
            with open(os.path.join(d, name), "wb") as f:
                f.write(data)
            files[os.path.join(d, name)] = data
    with open(os.path.join(remote, "TPP_GAME_DATA"), "wb") as f:
        f.write(b"untouched")
    return files


def run(*args):
    buf = io.StringIO()
    with redirect_stdout(buf):
        rc = s.main(list(args))
    return rc, buf.getvalue()


def main():
    root = tempfile.mkdtemp(prefix="instsaves-")
    try:
        acc = os.path.join(root, "acc")
        bdir = os.path.join(root, "backup")
        originals = make_account(acc)

        rc, out = run("--account-dir", acc, "--id", "2", "--backup-dir", bdir)
        check(rc == 0 and "OK: 9 files" in out, "creates 9 files (3 remote, 6 local)")
        check(all(open(p, "rb").read() == d for p, d in originals.items()), "originals unchanged")
        for n in s.NAMES:
            for rel in (os.path.join("311340", "remote", "DYN2_" + n), os.path.join("287700", "local", "DYN2_" + n + "1")):
                data = open(os.path.join(acc, rel), "rb").read()
                check(s.checksum_ok(s.xor_stream("DYN2_" + n, data)), "%s decodes with its own name" % rel)
                check(not s.checksum_ok(s.xor_stream("DYNAMITE_" + n, data)), "%s no longer decodes with the old name" % rel)
        zips = os.listdir(bdir)
        check(len(zips) == 1 and len(zipfile.ZipFile(os.path.join(bdir, zips[0])).namelist()) == 10, "backup zip has all 10 files")

        rc, out = run("--account-dir", acc, "--id", "2", "--backup-dir", bdir)
        check(rc == 1 and "already there" in out, "refuses to overwrite")

        # a corrupt source: nothing may be written
        acc2 = os.path.join(root, "acc2")
        make_account(acc2)
        with open(os.path.join(acc2, "287700", "local", "DYNAMITE_CONFIG_DATA1"), "r+b") as f:
            f.seek(200)
            f.write(b"\x00\x01\x02")
        try:
            run("--account-dir", acc2, "--id", "3", "--backup-dir", bdir)
            check(False, "corrupt source stops the run")
        except ValueError:
            check(True, "corrupt source stops the run")
        check(not any(f.startswith("DYN3_") for d in ("311340/remote", "287700/local") for f in os.listdir(os.path.join(acc2, d))),
              "nothing written when one file is bad")

        rc, out = run("--account-dir", os.path.join(root, "nowhere"), "--id", "2", "--backup-dir", bdir)
        check(rc == 1 and "no DYNAMITE_GAME_DATA" in out, "wrong account dir is reported")
        rc, out = run("--account-dir", acc, "--id", "..\\x", "--backup-dir", bdir)
        check(rc == 2, "instance id with path characters refused")
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print("ALL OK" if not fails else "FAILED %d" % len(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
