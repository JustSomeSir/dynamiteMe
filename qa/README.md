# QA: bot-driven co-op tests for two game copies on one PC

Test-only. Nothing here is active in a normal install.

| Piece | Where | What |
|---|---|---|
| Bridge server | `hook/src/QABridge*.{h,cpp}`, `hook/src/coopbridge/` (coop-anything, MIT) | Starts only when `coopqa.ini` sits next to `mgsvtpp.exe`; loopback only; endpoint in `dynamite/qa-endpoint.json` |
| Lua pump + dispatcher | `moddata/standalone/script_loader.lua`, `qa/lua/coopqa.lua` (copy to `<game dir>\lua\`) | `script_loader.Update` runs `coopqa.Update` every frame, which pumps the bridge on the game's Lua thread |
| Runner | `qa/run_qa.py`, `qa/bridge_client.py`, `qa/qa_config.json` | Starts master server, host, client; brings both into one mission; asserts; prints `RESULT: PASS/FAIL` |
| Setup | `qa/prepare.ps1` | Builds `dynamite.exe` from a source package and installs it into both test copies |

Tests: `connect` (both copies in one 2-player session, roles right), `positions` (each side sees both
players where the other side says they are), `stability` (soak: both alive, 2 members). Twins:
client never connects (`connect` must fail), swapped player indices (`positions` must fail).

Each test copy also needs (see games/metal-gear-solid-v in coop-anything): `steam_proxy.ini` +
`steamshim.dll` (tools/steamproxy) and, for copy 2, `dynamite/instance.txt`.

## Tests without the game (Linux)

```
python3 -I qa/tests/test_qabridge.py --lua-src <lua 5.1 src> --bridge-client <coop-anything/tools/bridge>
python3 -I qa/tests/test_runner_sim.py --lua-src <lua 5.1 src>
lua5.1 qa/tests/test_coopqa.lua qa/lua
```

## Known gaps
- Title screen: the runner presses CONTINUE itself (`CoopQA.pressContinue()` calls the title menu's own
  `OnSelectContinue`) and waits for sequence `Seq_Game_MainGame` with `gvars.ini_isTitleMode == false`; the title
  runs over the loaded mission, so mission code and heliSpace alone cannot tell it from the helicopter. Other title
  steps (popups) are not automated: after 30 s in one step the runner asks a person. `"title_wait": "enter"` in the
  config brings back the fully manual title. `probe_title.py` records title steps and handlers.
  Master server output goes to `runs/<run>/master-server.log`.
- No enemy-sync test yet (needs a Lua way to damage a soldier on the host).
