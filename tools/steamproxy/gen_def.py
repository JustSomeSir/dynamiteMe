"""Make exports.def for the steam_api64.dll proxy from `llvm-objdump -p steam_api64.dll` output.

Every export is forwarded to steam_api64_real.dll with its original ordinal, except the few the
proxy implements itself (OVERRIDDEN). Usage:
    llvm-objdump -p <game dir>/steam_api64.dll > exports.txt
    python3 -I gen_def.py exports.txt > exports.def
"""

import re
import sys

OVERRIDDEN = {
    "SteamNetworking",
    "SteamUser",
    "SteamAPI_RegisterCallback",
    "SteamAPI_UnregisterCallback",
    "SteamAPI_RunCallbacks",
    # logged pass-throughs (the rest of mgsvtpp.exe's steam_api64 imports)
    "SteamUserStats",
    "SteamUtils",
    "SteamFriends",
    "SteamRemoteStorage",
    "SteamApps",
    "SteamAPI_Init",
    "SteamAPI_Shutdown",
    "SteamAPI_RegisterCallResult",
    "SteamAPI_UnregisterCallResult",
}

ROW = re.compile(r"^\s*(\d+)\s+0x[0-9a-fA-F]+\s+(\S+)\s*$")


def main(path: str) -> int:
    rows = []
    in_table = False
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("Export Table"):
                in_table = True
                continue
            if in_table:
                m = ROW.match(line)
                if m:
                    rows.append((int(m.group(1)), m.group(2)))
    if not rows:
        print("no exports found", file=sys.stderr)
        return 1
    missing = OVERRIDDEN - {name for _, name in rows}
    if missing:
        print(f"real DLL lacks overridden exports: {sorted(missing)}", file=sys.stderr)
        return 1

    print("LIBRARY steam_api64.dll")
    print("EXPORTS")
    for ordinal, name in rows:
        if name in OVERRIDDEN:
            print(f"    {name} @{ordinal}")
        else:
            print(f"    {name}=steam_api64_real.{name} @{ordinal}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
