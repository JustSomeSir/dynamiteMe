# steamproxy: two game copies, one Steam account, Steam P2P over localhost

Test-only. Lets two copies of MGSV on one PC, both logged into the same Steam account, talk to each
other through the game's own `ISteamNetworking005` P2P calls, which normally can't connect an
account to itself. Never install it in a real game install.

| File | What |
|---|---|
| `p2pcore.{h,cpp}` | Portable transport (UDP on 127.0.0.1), Steam P2P semantics: channels, truncation, held packets + session request until accept |
| `proxy.cpp`, `thunks.S`, `exports.def` | `steam_api64.dll` proxy: forwards all 747 exports to `steam_api64_real.dll`, wraps `SteamNetworking()` and (with a fake id) `SteamUser()`, raises `P2PSessionRequest_t` (1202) in `SteamAPI_RunCallbacks` |
| `gen_def.py` | Makes `exports.def` from `llvm-objdump -p` of the real DLL |
| `test_p2pcore.cpp` | Linux test of the transport (`run_tests.sh`, ASan/UBSan/TSan) |
| `selftest.cpp` | Windows self-test with real Steam: ABI of `GetSteamID`, wrappers, callback delivery |

## Install in a test copy

1. Rename `<game dir>\steam_api64.dll` to `steam_api64_real.dll`.
2. Copy the built proxy in as `steam_api64.dll`.
3. Add `<game dir>\steam_proxy.ini`:

```
# copy 1 (host): real SteamID
listen_port=27150
peer_port=27151
```
```
# copy 2 (client): pretend to be another account
listen_port=27151
peer_port=27150
fake_steam_id=76561202255233008
```

No `steam_proxy.ini` = the proxy forwards everything unchanged. Each process logs to
`<game dir>\steamproxy-<pid>.log`.
