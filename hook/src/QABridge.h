#ifndef HOOK_QABRIDGE_H
#define HOOK_QABRIDGE_H

// Test-only RPC bridge (coop-anything tools/bridge protocol) for bot-driven QA.
//
// Off unless <game dir>/coopqa.ini exists. Then the hook starts a loopback-only server at load and
// writes its endpoint to dynamite/qa-endpoint.json. Requests run on the game's Lua thread: the
// test Lua module (lua/coopqa.lua) calls Dynamite.QAPump(dispatch) once per frame from
// script_loader.Update, and every request is answered by calling dispatch(kind, arg) in Lua.

struct lua_State;

namespace Dynamite::QA {
    // Starts the server if coopqa.ini exists. Call once from the hook constructor.
    void Setup();
    bool Enabled();
    // Lua: Dynamite.QAPump(dispatch). Runs pending requests now, on this Lua thread.
    int l_QAPump(lua_State *L);
    // Lua: Dynamite.QAEvent(type, text). Adds an event for the `events` method.
    int l_QAEvent(lua_State *L);
    // Lua: Dynamite.QAMemberCount() -> int, Dynamite.QAPlayerPos(i) -> x, y, z
    int l_QAMemberCount(lua_State *L);
    int l_QAPlayerPos(lua_State *L);
    // test copies: instance id from dynamite/instance.txt, "" for normal play
    int l_InstanceId(lua_State *L);
    // test-only memory probes for objects whose position Lua can't ask for (MemScanCore.h)
    int l_QAScan(lua_State *L);
    int l_QAVec(lua_State *L);
} // namespace Dynamite::QA

#endif
