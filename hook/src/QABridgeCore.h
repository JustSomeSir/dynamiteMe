#ifndef HOOK_QABRIDGECORE_H
#define HOOK_QABRIDGECORE_H

// Game-independent core of the QA bridge: the coopbridge adapter that answers requests by calling
// the Lua dispatcher. The Lua C functions come in through LuaApi, so the hook passes the game's
// function pointers and the host test (qa/tests/qabridge_host.cpp) passes real Lua 5.1.

#include "coopbridge/server.h"

#include <cstddef>
#include <string>
#include <vector>

struct lua_State;

namespace Dynamite::QA {

    struct LuaApi {
        void (*pushvalue)(lua_State *, int);
        void (*pushstring)(lua_State *, const char *);
        int (*pcall)(lua_State *, int, int, int);
        const char *(*checklstring)(lua_State *, int, size_t *);
        void (*remove)(lua_State *, int);
        void (*trace)(const char *) = nullptr; // optional step log (the game hangs somewhere after a reply)
    };

    class LuaAdapter : public coopbridge::Adapter {
      public:
        explicit LuaAdapter(const LuaApi &api) : api_(api) {}

        // Set by the pump for the duration of Server::Pump(); dispatchIndex is the stack slot of
        // coopqa.dispatch on that state.
        void Bind(lua_State *L, int dispatchIndex) {
            L_ = L;
            dispatchIndex_ = dispatchIndex;
        }
        void Unbind() { L_ = nullptr; }

        coopbridge::Capabilities capabilities() const override {
            coopbridge::Capabilities c;
            c.exec = c.eval = c.state = true;
            return c;
        }

        coopbridge::ExecResult Exec(const std::string &line, const std::string &) override { return {true, Call("exec", line)}; }

        coopbridge::json::Value Eval(const std::string &expr, const std::string &) override { return Parse(Call("eval", expr)); }

        coopbridge::json::Value State(const std::vector<std::string> &sections) override {
            std::string joined;
            for (const auto &s : sections) {
                if (!joined.empty()) joined += ",";
                joined += s;
            }
            return Parse(Call("state", joined));
        }

        // dispatch(kind, arg) -> "1"+JSON or "0"+message. The Lua stack is left as it was.
        std::string Call(const char *kind, const std::string &arg) {
            if (L_ == nullptr) {
                throw coopbridge::Error(-32011, "no Lua state (QAPump is not running)");
            }
            api_.pushvalue(L_, dispatchIndex_);
            api_.pushstring(L_, kind);
            api_.pushstring(L_, arg.c_str());
            Trace("call: pcall");
            const int rc = api_.pcall(L_, 2, 1, 0);
            Trace("call: checklstring");
            size_t n = 0;
            const char *s = api_.checklstring(L_, -1, &n); // a string either way (see coopqa.lua)
            std::string out(s ? s : "", s ? n : 0);
            Trace("call: remove");
            api_.remove(L_, -1);
            Trace(("call: reply " + out.substr(0, 300)).c_str());
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

      private:
        coopbridge::json::Value Parse(const std::string &text) {
            coopbridge::json::Value v;
            std::string err;
            const bool ok = coopbridge::json::parse(text, v, err);
            Trace(ok ? "parse: ok" : ("parse: failed: " + err).c_str());
            if (!ok) {
                throw coopbridge::Error(-32015, "coopqa returned invalid JSON: " + err);
            }
            return v;
        }

        void Trace(const char *step) const {
            if (api_.trace) api_.trace(step);
        }

        LuaApi api_;
        lua_State *L_ = nullptr;
        int dispatchIndex_ = 1;
    };

} // namespace Dynamite::QA

#endif
