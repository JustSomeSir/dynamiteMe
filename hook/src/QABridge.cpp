#include "QABridge.h"

#include "DynamiteHook.h"
#include "Version.h"
#include "dynamite.h"
#include "lua/lauxlib.h"
#include "lua/lua.h"
#include "spdlog/spdlog.h"

#include "MemScanCore.h"
#include "QABridgeCore.h"
#include "SecondInstance.h"

#include <windows.h>

#include <atomic>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace Dynamite::QA {
    namespace {
        bool g_enabled = false;

        // The game's Lua C functions (1.0.15.4 addresses); see QABridgeCore.h.
        void PushValue(lua_State *L, int i) { lua_pushvalue(L, i); }
        void PushString(lua_State *L, const char *str) { lua_pushstring(L, str); }
        int PCall(lua_State *L, int a, int r, int e) { return lua_pcall(L, a, r, e); }
        const char *CheckLString(lua_State *L, int i, size_t *n) { return luaL_checklstring(L, i, n); }
        void Remove(lua_State *L, int i) { lua_remove(L, i); }

        // Step log for the first requests: the game froze right after the first served reply
        // (2026-11-09) and the Lua side showed the reply was built. These lines show where it stops.
        std::atomic<int> g_traceLeft{60};
        void Trace(const char *step) {
            if (g_traceLeft.fetch_sub(1) > 0) spdlog::info("qa trace: {}", step);
        }

        LuaAdapter g_adapter(LuaApi{&PushValue, &PushString, &PCall, &CheckLString, &Remove, &Trace});

        // Do C++ exceptions work in this process? The bridge reports every error by throwing.
        void ExceptionSelfTest(const char *where) {
            try {
                throw coopbridge::Error(-1, "self-test");
            } catch (const std::exception &e) {
                spdlog::info("qa bridge: exception self-test ({}) caught: {}", where, e.what());
            }
        }
        coopbridge::Server *g_server = nullptr; // leaked on purpose: lives until process exit

        int ReadPort() {
            std::ifstream f("coopqa.ini");
            std::string line;
            while (std::getline(f, line)) {
                if (line.rfind("port=", 0) == 0) {
                    try {
                        const int p = std::stoi(line.substr(5));
                        if (p >= 0 && p <= 65535) return p;
                    } catch (...) {
                    }
                }
            }
            return 0; // free port; the endpoint file tells the runner which
        }
    } // namespace

    bool Enabled() { return g_enabled; }

    void Setup() {
        if (GetFileAttributesW(L"coopqa.ini") == INVALID_FILE_ATTRIBUTES) {
            return; // normal play: no server, no thread, nothing listens
        }
        coopbridge::Options opt;
        opt.game = "mgsvtpp-dynamite";
        opt.build = SOFTWARE_VERSION;
        opt.tcpPort = ReadPort();
        opt.endpointFile = "dynamite\\qa-endpoint.json";
        opt.defaultTimeoutMs = 30000;
        g_server = new coopbridge::Server();
        if (!g_server->Start(opt, g_adapter)) {
            spdlog::error("qa bridge: start failed: {}", g_server->lastError());
            return;
        }
        g_enabled = true;
        ExceptionSelfTest("setup");
        g_server->Register(
            "qa.stats",
            [](const coopbridge::json::Value &) {
                const auto s = g_server->stats();
                auto v = coopbridge::json::Value::object();
                v.set("frames", (double)s.frames);
                v.set("served", (double)s.served);
                v.set("mainLoopAgeMs", (double)s.mainLoopAgeMs);
                return v;
            },
            false);
        spdlog::info("qa bridge: listening, endpoint file dynamite\\qa-endpoint.json, port setting {}", opt.tcpPort);
    }

    int l_QAPump(lua_State *L) {
        if (!g_enabled) return 0;
        static bool firstPump = true;
        if (firstPump) {
            firstPump = false;
            ExceptionSelfTest("first pump, main thread");
            // Every Lua function the pump calls must be bound (SetFuncPtrs runs after Setup): lua_remove
            // had an address but was never assigned, and calling the null pointer froze the game (2026-11-09).
            if (!lua_pushvalue || !lua_pushstring || !lua_pcall || !luaL_checklstring || !lua_remove || !lua_pushinteger ||
                !lua_pushnumber || !luaL_checkinteger) {
                spdlog::error("qa bridge: a Lua function pointer is not set for this game version; bridge disabled");
                g_enabled = false;
                return 0;
            }
        }
        g_adapter.Bind(L, 1); // Dynamite.QAPump(dispatch): dispatch is argument 1
        const auto servedBefore = g_server->stats().served;
        try {
            g_server->Pump();
        } catch (const std::exception &e) {
            spdlog::error("qa bridge: pump: {}", e.what());
        }
        g_adapter.Unbind();
        if (g_server->stats().served != servedBefore) Trace("pump: returned after serving");
        return 0;
    }

    int l_QAEvent(lua_State *L) {
        if (!g_enabled) return 0;
        size_t n1 = 0, n2 = 0;
        const char *type = luaL_checklstring(L, 1, &n1);
        const char *text = luaL_checklstring(L, 2, &n2);
        g_server->PushEvent(std::string(type, n1), coopbridge::json::Value(std::string(text, n2)));
        return 0;
    }

    int l_QAMemberCount(lua_State *L) {
        lua_pushinteger(L, g_hook->dynamiteCore.GetMemberCount());
        return 1;
    }

    namespace {
        // Reads this process's own memory without faulting on unmapped or guarded pages.
        size_t SafeRead(uintptr_t addr, void *out, size_t n) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0) return 0;
            if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return 0;
            const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (addr >= end) return 0;
            if (n > end - addr) n = end - addr;
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), out, n, &got)) return 0;
            return got;
        }
    } // namespace

    // Dynamite.QAScan(objectId, x, y, z, radius, depth, window, maxNodes) -> text, see MemScanCore.h
    int l_QAScan(lua_State *L) {
        if (!g_enabled) return 0;
        const auto id = static_cast<uint16_t>(luaL_checkinteger(L, 1));
        ScanOptions o;
        o.x = static_cast<float>(luaL_checknumber(L, 2));
        o.y = static_cast<float>(luaL_checknumber(L, 3));
        o.z = static_cast<float>(luaL_checknumber(L, 4));
        o.radius = static_cast<float>(luaL_checknumber(L, 5));
        o.depth = static_cast<int>(luaL_checkinteger(L, 6));
        o.window = static_cast<size_t>(luaL_checkinteger(L, 7));
        o.maxNodes = static_cast<size_t>(luaL_checkinteger(L, 8));
        if (o.depth < 0 || o.depth > 8 || o.window < 12 || o.window > 0x10000 || o.maxNodes > 200000) {
            lua_pushstring(L, "bad arguments");
            return 1;
        }
        if (const auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))) {
            const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(exe);
            const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(exe + dos->e_lfanew);
            o.skipLo = exe; // code and vtables: no positions there
            o.skipHi = exe + nt->OptionalHeader.SizeOfImage;
        }
        const auto root = reinterpret_cast<uintptr_t>(FindGameObjectWithID(id));
        char head[64];
        snprintf(head, sizeof(head), "root=%p\n", reinterpret_cast<void *>(root));
        const std::string out = head + ScanForVec3(root, o, &SafeRead);
        lua_pushstring(L, out.c_str());
        return 1;
    }

    // Dynamite.QAVec(objectId, "40>10@50") -> x, y, z (nothing if the path can't be read)
    int l_QAVec(lua_State *L) {
        if (!g_enabled) return 0;
        const auto id = static_cast<uint16_t>(luaL_checkinteger(L, 1));
        size_t n = 0;
        const char *path = luaL_checklstring(L, 2, &n);
        float v[3];
        const auto root = reinterpret_cast<uintptr_t>(FindGameObjectWithID(id));
        if (!ReadVec3ByPath(root, std::string(path, n), &SafeRead, v)) return 0;
        lua_pushnumber(L, v[0]);
        lua_pushnumber(L, v[1]);
        lua_pushnumber(L, v[2]);
        return 3;
    }

    int l_InstanceId(lua_State *L) {
        lua_pushstring(L, InstanceIdUtf8().c_str());
        return 1;
    }

    int l_QAPlayerPos(lua_State *L) {
        const auto i = luaL_checkinteger(L, 1);
        const auto p = DynamiteCore::GetPlayerPosition((int)i);
        lua_pushnumber(L, p.x);
        lua_pushnumber(L, p.y);
        lua_pushnumber(L, p.z);
        return 3;
    }
} // namespace Dynamite::QA
