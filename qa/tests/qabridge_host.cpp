// Host stand-in for the game: real coopbridge server + real Lua 5.1 + qa/lua/coopqa.lua with stubbed
// game APIs (qa/tests/stubs.lua), driven through hook/src/QABridgeCore.h exactly like the hook does.
//
//   qabridge_host <qa/lua dir> <stubs.lua> <endpoint file>
//
// A "frame" calls coopqa.Update() every 10 ms. The test (test_qabridge.py) talks to it with
// coop-anything's bridge_client.py. Extra knobs for the test, set from Lua through exec:
//   QA_HOST.freezeMs = N  -> the next frame sleeps N ms (a stuck main thread)
//   QA_HOST.quit = true   -> exit 0
// Every QAPump checks that the Lua stack is the same before and after the pump; mismatches are
// counted in QA_HOST.stackErrors.
extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include "QABridgeCore.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace Dynamite::QA;

static void PushValue(lua_State *L, int i) { lua_pushvalue(L, i); }
static void PushString(lua_State *L, const char *s) { lua_pushstring(L, s); }
static int PCall(lua_State *L, int a, int r, int e) { return lua_pcall(L, a, r, e); }
static const char *CheckLString(lua_State *L, int i, size_t *n) { return luaL_checklstring(L, i, n); }
static void Remove(lua_State *L, int i) { lua_remove(L, i); }

static LuaAdapter g_adapter(LuaApi{&PushValue, &PushString, &PCall, &CheckLString, &Remove});
static coopbridge::Server g_server;

static int HostTableInt(lua_State *L, const char *key) {
    lua_getglobal(L, "QA_HOST");
    lua_getfield(L, -1, key);
    const int v = (int)lua_tointeger(L, -1);
    lua_pop(L, 2);
    return v;
}

static void HostTableSetInt(lua_State *L, const char *key, int v) {
    lua_getglobal(L, "QA_HOST");
    lua_pushinteger(L, v);
    lua_setfield(L, -2, key);
    lua_pop(L, 1);
}

// Same three lines as Dynamite::QA::l_QAPump in hook/src/QABridge.cpp, plus the stack check.
static int l_QAPump(lua_State *L) {
    const int before = lua_gettop(L);
    g_adapter.Bind(L, 1);
    try {
        g_server.Pump();
    } catch (const std::exception &e) {
        std::fprintf(stderr, "pump: %s\n", e.what());
    }
    g_adapter.Unbind();
    if (lua_gettop(L) != before) {
        std::fprintf(stderr, "stack changed in pump: %d -> %d\n", before, lua_gettop(L));
        lua_settop(L, before);
        HostTableSetInt(L, "stackErrors", HostTableInt(L, "stackErrors") + 1);
    }
    return 0;
}

static int l_QAEvent(lua_State *L) {
    g_server.PushEvent(luaL_checkstring(L, 1), coopbridge::json::Value(std::string(luaL_checkstring(L, 2))));
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s <qa lua dir> <stubs.lua> <endpoint file>\n", argv[0]);
        return 2;
    }
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    const std::string path = std::string(argv[1]) + "/?.lua";
    lua_getglobal(L, "package");
    lua_pushstring(L, path.c_str());
    lua_setfield(L, -2, "path");
    lua_pop(L, 1);

    if (luaL_dofile(L, argv[2]) != 0) { // stubs: Dynamite.*, vars, TppMission, QA_HOST
        std::fprintf(stderr, "stubs: %s\n", lua_tostring(L, -1));
        return 2;
    }
    lua_getglobal(L, "Dynamite");
    lua_pushcfunction(L, l_QAPump);
    lua_setfield(L, -2, "QAPump");
    lua_pushcfunction(L, l_QAEvent);
    lua_setfield(L, -2, "QAEvent");
    lua_pop(L, 1);

    if (luaL_dostring(L, "QA_MODULE = require('coopqa')") != 0) {
        std::fprintf(stderr, "require coopqa: %s\n", lua_tostring(L, -1));
        return 2;
    }

    coopbridge::Options opt;
    opt.game = "qabridge-host";
    opt.build = "test";
    opt.tcpPort = 0;
    opt.endpointFile = argv[3];
    opt.defaultTimeoutMs = 2000;
    opt.slackMs = 500;
    if (!g_server.Start(opt, g_adapter)) {
        std::fprintf(stderr, "server: %s\n", g_server.lastError().c_str());
        return 2;
    }

    for (;;) {
        lua_getglobal(L, "QA_MODULE");
        lua_getfield(L, -1, "Update");
        if (lua_pcall(L, 0, 0, 0) != 0) {
            std::fprintf(stderr, "Update: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        if (lua_gettop(L) != 0) {
            std::fprintf(stderr, "stack not empty after frame: %d\n", lua_gettop(L));
            return 3;
        }

        lua_getglobal(L, "QA_HOST");
        lua_getfield(L, -1, "quit");
        const bool quit = lua_toboolean(L, -1);
        lua_pop(L, 2);
        if (quit) break;

        const int freeze = HostTableInt(L, "freezeMs");
        if (freeze > 0) {
            HostTableSetInt(L, "freezeMs", 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(freeze));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    g_server.Stop();
    lua_close(L);
    return 0;
}
