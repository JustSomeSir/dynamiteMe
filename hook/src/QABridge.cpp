#include "QABridge.h"

#include "DynamiteHook.h"
#include "Version.h"
#include "dynamite.h"
#include "lua/lauxlib.h"
#include "lua/lua.h"
#include "spdlog/spdlog.h"

#include "coopbridge/server.h"

#include <windows.h>

#include <atomic>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace Dynamite::QA {
    namespace {
        bool g_enabled = false;
        lua_State *g_L = nullptr; // set only while QAPump runs; arg 1 on its stack is the dispatcher

        // Calls dispatch(kind, arg) in Lua and returns its string result.
        // Contract with lua/coopqa.lua: dispatch never raises and always returns a string whose
        // first byte is '1' (ok) or '0' (error), followed by JSON (ok) or a message (error).
        // Only Lua C functions with 1.0.15.4 addresses are used: pushvalue, pushstring, pcall,
        // checklstring, remove. The stack is left as it was.
        std::string CallDispatch(const char *kind, const std::string &arg) {
            lua_State *L = g_L;
            if (L == nullptr) {
                throw coopbridge::Error(-32011, "no Lua state (QAPump is not running)");
            }
            lua_pushvalue(L, 1);           // dispatch
            lua_pushstring(L, kind);       // kind
            lua_pushstring(L, arg.c_str()); // arg
            const int rc = lua_pcall(L, 2, 1, 0);
            size_t n = 0;
            const char *s = luaL_checklstring(L, -1, &n); // a string in both outcomes (see contract)
            std::string out(s ? s : "", s ? n : 0);
            lua_remove(L, -1);
            if (rc != 0) {
                throw coopbridge::Error(-32012, "Lua error: " + out);
            }
            if (out.empty() || (out[0] != '1' && out[0] != '0')) {
                throw coopbridge::Error(-32013, "bad reply from coopqa.dispatch");
            }
            if (out[0] == '0') {
                throw coopbridge::Error(-32014, out.substr(1));
            }
            return out.substr(1);
        }

        coopbridge::json::Value ParseJson(const std::string &text) {
            coopbridge::json::Value v;
            std::string err;
            if (!coopbridge::json::parse(text, v, err)) {
                throw coopbridge::Error(-32015, "coopqa returned invalid JSON: " + err);
            }
            return v;
        }

        class LuaAdapter : public coopbridge::Adapter {
          public:
            coopbridge::Capabilities capabilities() const override {
                coopbridge::Capabilities c;
                c.exec = c.eval = c.state = true;
                return c;
            }

            // A Lua chunk; its return values (JSON-encoded by coopqa) are the output.
            coopbridge::ExecResult Exec(const std::string &line, const std::string &) override { return {true, CallDispatch("exec", line)}; }

            // A Lua expression; the result is the JSON value.
            coopbridge::json::Value Eval(const std::string &expr, const std::string &) override { return ParseJson(CallDispatch("eval", expr)); }

            coopbridge::json::Value State(const std::vector<std::string> &sections) override {
                std::string joined;
                for (const auto &s : sections) {
                    if (!joined.empty()) joined += ",";
                    joined += s;
                }
                return ParseJson(CallDispatch("state", joined));
            }
        };

        LuaAdapter g_adapter;
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
        g_L = L;
        try {
            g_server->Pump();
        } catch (const std::exception &e) {
            spdlog::error("qa bridge: pump: {}", e.what());
        }
        g_L = nullptr;
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

    int l_QAPlayerPos(lua_State *L) {
        const auto i = luaL_checkinteger(L, 1);
        const auto p = DynamiteCore::GetPlayerPosition((int)i);
        lua_pushnumber(L, p.x);
        lua_pushnumber(L, p.y);
        lua_pushnumber(L, p.z);
        return 3;
    }
} // namespace Dynamite::QA
