"""Gives a test copy of MGSV its own save files (instance id from <copy>/dynamite/instance.txt).

With an instance id, TppDefine.lua names the saves DYN<id>_GAME_DATA etc. instead of DYNAMITE_*, so the
two copies on one PC stop loading and overwriting the same save. This script creates those files once,
from the existing DYNAMITE_* saves:

    python instance_saves.py --account-dir "E:\\Steam\\userdata\\<steam id>" --id 2 --backup-dir E:\\coop-mgsv\\local

What it touches:
  reads   <account>\\311340\\remote\\DYNAMITE_{GAME,CONFIG,PERSONAL}_DATA   (never changed)
          <account>\\287700\\local\\DYNAMITE_*_DATA{0,1}                    (never changed)
  writes  a zip backup of both folders into --backup-dir, first
          the same files under the DYN<id>_ name, next to the originals
It refuses to overwrite a DYN<id>_ file that already exists.

Save files are XOR-encoded with a stream keyed by md5(file name) (fox::FoxGameSaveCommon::DecodeSaveData,
as in unknown321/squib save.Decode, which Dynamite's savebackup uses the same way). A renamed save must be
re-encoded for its new name. After decoding, bytes 0..15 are md5 of the rest: checked before writing.
Local copies (…0, …1) are keyed by the base name, as Dynamite's CreateLocalSaves writes them.
"""

import argparse
import hashlib
import os
import struct
import sys
import time
import zipfile

NAMES = ("GAME_DATA", "CONFIG_DATA", "PERSONAL_DATA")
M32 = 0xFFFFFFFF


def xor_stream(key, data):
    """Encode and decode are the same operation."""
    out = bytearray(data)
    state = struct.unpack_from("<I", hashlib.md5(key.encode()).digest())[0]
    n4 = len(out) - len(out) % 4
    for i in range(0, n4, 4):
        state ^= (state << 13) & M32
        state ^= state >> 7
        state ^= (state << 5) & M32
        block = struct.unpack_from("<I", out, i)[0]
        struct.pack_into("<I", out, i, block ^ state)
    rem = len(out) % 4
    if rem:
        state ^= (state << 13) & M32
        state ^= state >> 7
        tmp = state ^ ((state << 5) & M32)
        for i in range(rem):
            out[n4 + i] ^= (tmp >> (8 * i)) & 0xFF
    return bytes(out)


def checksum_ok(plain):
    return len(plain) > 16 and hashlib.md5(plain[16:]).digest() == plain[:16]


def rekey(data, old_key, new_key):
    plain = xor_stream(old_key, data)
    if not checksum_ok(plain):
        raise ValueError("%s: checksum does not match after decoding (not a save, or keyed differently)" % old_key)
    out = xor_stream(new_key, plain)
    if xor_stream(new_key, out) != plain:
        raise AssertionError("re-encoding is not reversible")
    return out


def plan(account_dir, instance_id):
    """(source path, target path, old key, new key) for every file that exists."""
    prefix = "DYN%s_" % instance_id
    jobs = []
    remote = os.path.join(account_dir, "311340", "remote")
    local = os.path.join(account_dir, "287700", "local")
    for n in NAMES:
        old, new = "DYNAMITE_" + n, prefix + n
        jobs.append((os.path.join(remote, old), os.path.join(remote, new), old, new))
        for slot in ("0", "1"):
            jobs.append((os.path.join(local, old + slot), os.path.join(local, new + slot), old, new))
    return [j for j in jobs if os.path.exists(j[0])]


def backup(account_dir, backup_dir):
    os.makedirs(backup_dir, exist_ok=True)
    path = os.path.join(backup_dir, "saves-before-instance-%s.zip" % time.strftime("%Y%m%d-%H%M%S"))
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for sub in (os.path.join("311340", "remote"), os.path.join("287700", "local")):
            root = os.path.join(account_dir, sub)
            for dirpath, _, files in os.walk(root):
                for f in files:
                    full = os.path.join(dirpath, f)
                    z.write(full, os.path.relpath(full, account_dir))
    return path


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--account-dir", required=True, help="Steam userdata folder of the account")
    ap.add_argument("--id", required=True, help="instance id, as in <copy>/dynamite/instance.txt (e.g. 2)")
    ap.add_argument("--backup-dir", required=True)
    a = ap.parse_args(argv)
    if not a.id.isalnum() or len(a.id) > 3:
        print("instance id must be 1-3 letters or digits")
        return 2
    jobs = plan(a.account_dir, a.id)
    if not any(j[2] == "DYNAMITE_GAME_DATA" and "remote" in j[0] for j in jobs):
        print("no DYNAMITE_GAME_DATA in %s\\311340\\remote: wrong account dir, or Dynamite never ran" % a.account_dir)
        return 1
    exists = [j[1] for j in jobs if os.path.exists(j[1])]
    if exists:
        print("already there, nothing changed:\n  " + "\n  ".join(exists))
        return 1
    # decode and check everything before writing anything
    out = []
    for src, dst, old, new in jobs:
        with open(src, "rb") as f:
            out.append((src, dst, rekey(f.read(), old, new)))
    print("backup:", backup(a.account_dir, a.backup_dir))
    for src, dst, data in out:
        with open(dst, "wb") as f:
            f.write(data)
        print("%s -> %s  (%d bytes, sha256 %s)" % (os.path.basename(src), os.path.basename(dst), len(data),
                                                  hashlib.sha256(data).hexdigest()[:12]))
    print("OK: %d files" % len(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
