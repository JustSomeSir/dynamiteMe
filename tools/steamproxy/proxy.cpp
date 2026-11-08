// steam_api64.dll proxy for TEST COPIES of MGSV only: lets two copies on one PC, logged into the
// same Steam account, talk Steam P2P to each other over localhost UDP.
//
// Install (per test copy): rename the game's steam_api64.dll to steam_api64_real.dll, put this
// DLL in as steam_api64.dll, add steam_proxy.ini (see README.md). Without steam_proxy.ini the
// proxy forwards everything unchanged.
//
// Everything is forwarded to steam_api64_real.dll (exports.def) except:
//   SteamNetworking()            -> wrapper of ISteamNetworking005 whose P2P methods use P2PCore
//   SteamUser()                  -> wrapper whose GetSteamID returns fake_steam_id (if set)
//   SteamAPI_RegisterCallback /  -> remembered for P2PSessionRequest_t (1202), then forwarded
//   SteamAPI_UnregisterCallback
//   SteamAPI_RunCallbacks        -> forwarded, then our P2PSessionRequest_t are delivered
//
// ABI notes (MSVC x64, which the game uses): a virtual call passes `this` in rcx and the rest as a
// normal x64 call, so the vtable entries below are plain functions with `self` first. CSteamID (8
// bytes) is passed by value in a register. A member function returning CSteamID returns it through
// a hidden pointer passed after `this`.

#include "p2pcore.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

extern "C" void *const sp_fwd_table[32]; // thunks.S: slot i -> real->vtbl[i] with `this` swapped

namespace {
    using steamproxy::P2PCore;

    constexpr int kP2PSessionRequest = 1202; // k_iSteamNetworkingCallbacks + 2
    constexpr int kNetVtableSize = 22;       // ISteamNetworking005
    constexpr int kUserVtableCopy = 32;      // >= ISteamUser018 method count; unused slots are never called
    constexpr int kUserGetSteamID = 2;       // GetHSteamUser, BLoggedOn, GetSteamID

    HMODULE g_real = nullptr;
    std::wstring g_dir;

    // ---------------- log ----------------
    CRITICAL_SECTION g_logLock;
    HANDLE g_log = INVALID_HANDLE_VALUE;

    void Log(const char *fmt, ...) {
        if (g_log == INVALID_HANDLE_VALUE) return;
        const DWORD savedErr = GetLastError();
        char buf[1024];
        SYSTEMTIME st;
        GetLocalTime(&st);
        int n = snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u tid=%lu ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId());
        va_list ap;
        va_start(ap, fmt);
        n += vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap);
        va_end(ap);
        if (n > (int)sizeof(buf) - 3) n = sizeof(buf) - 3;
        buf[n++] = '\r';
        buf[n++] = '\n';
        EnterCriticalSection(&g_logLock);
        DWORD w;
        WriteFile(g_log, buf, n, &w, nullptr);
        LeaveCriticalSection(&g_logLock);
        SetLastError(savedErr);
    }

    // Plain WriteFile, no CRT: used before anything else can fail.
    void RawLog(const char *s) {
        if (g_log == INVALID_HANDLE_VALUE) return;
        DWORD w;
        WriteFile(g_log, s, (DWORD)strlen(s), &w, nullptr);
    }

    HMODULE g_module = nullptr;
    // Active only during DllMain: records where an exception happened (offset from module base).
    LONG CALLBACK InitCrashLogger(EXCEPTION_POINTERS *e) {
        char buf[200];
        const auto rip = (uintptr_t)e->ExceptionRecord->ExceptionAddress;
        snprintf(buf, sizeof(buf), "dllmain: exception 0x%08lx at %p (module+0x%llx)\r\n", e->ExceptionRecord->ExceptionCode, (void *)rip,
            (unsigned long long)(rip - (uintptr_t)g_module));
        RawLog(buf);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // ---------------- real DLL ----------------
    template <typename T> T Real(const char *name) {
        if (!g_real) {
            g_real = LoadLibraryW((g_dir + L"\\steam_api64_real.dll").c_str());
            if (!g_real) {
                Log("FATAL: cannot load steam_api64_real.dll (error %lu)", GetLastError());
                return nullptr;
            }
        }
        auto p = (T)(void *)GetProcAddress(g_real, name);
        if (!p) Log("FATAL: steam_api64_real.dll has no %s", name);
        return p;
    }

    // ---------------- config + core ----------------
    std::once_flag g_initOnce;
    bool g_enabled = false; // steam_proxy.ini found and valid
    uint64_t g_fakeId = 0;
    P2PCore *g_core = nullptr; // leaked on purpose: lives until process exit

    uint64_t RealSteamId();

    void Init() {
        std::call_once(g_initOnce, [] {
            std::ifstream f(std::filesystem::path(g_dir + L"\\steam_proxy.ini"));
            if (!f) {
                Log("no steam_proxy.ini: pure passthrough");
                return;
            }
            std::stringstream ss;
            ss << f.rdbuf();
            steamproxy::P2PConfig cfg;
            std::string err;
            if (!steamproxy::ParseConfig(ss.str(), &cfg, &g_fakeId, &err)) {
                Log("steam_proxy.ini invalid (%s): pure passthrough", err.c_str());
                return;
            }
            g_core = new P2PCore();
            if (!g_fakeId) g_core->SetIdProvider([] { return RealSteamId(); });
            if (!g_core->Start(cfg, [](const std::string &s) { Log("core: %s", s.c_str()); })) {
                Log("core failed to start: pure passthrough");
                return;
            }
            g_enabled = true;
            Log("enabled: fake_steam_id=%llu (0 = real id)", (unsigned long long)g_fakeId);
        });
    }

    // ---------------- callbacks (P2PSessionRequest_t) ----------------
    std::mutex g_cbMu;
    std::vector<void *> g_sessionCallbacks; // CCallbackBase* registered for 1202
    std::vector<uint64_t> g_undelivered;    // requests waiting for a registered callback

    using RunCb = void (*)(void *self, void *param);
    using SizeCb = int (*)(void *self);

    bool DeliverSessionRequest(uint64_t peer) {
        std::vector<void *> cbs;
        {
            std::lock_guard<std::mutex> g(g_cbMu);
            cbs = g_sessionCallbacks;
        }
        if (cbs.empty()) return false;
        struct {
            uint64_t steamIDRemote;
        } data{peer};
        for (void *cb : cbs) {
            void **vt = *(void ***)cb;
            const int size = ((SizeCb)vt[2])(cb);
            if (size != (int)sizeof(data)) {
                Log("callback %p for 1202 reports size %d, skipped", cb, size);
                continue;
            }
            Log("delivering P2PSessionRequest_t(%llu) to %p", (unsigned long long)peer, cb);
            // CCallback implements both Run overloads with the same body, so vtable[1] runs the
            // handler whether the compiler put Run(void*) first or second.
            ((RunCb)vt[1])(cb, &data);
        }
        return true;
    }

    // ---------------- ISteamNetworking005 wrapper ----------------
    struct NetWrap {
        void **vtbl;
        void *real;
    };
    void *g_netVtbl[kNetVtableSize];
    std::map<void *, NetWrap *> g_netWraps;
    std::mutex g_wrapMu;

    void **RealVt(NetWrap *w) { return *(void ***)w->real; }

    bool __cdecl Net_SendP2PPacket(NetWrap *, uint64_t id, const void *data, uint32_t size, int type, int channel) {
        return g_core->Send(id, data, size, type, channel);
    }
    bool __cdecl Net_IsP2PPacketAvailable(NetWrap *, uint32_t *size, int channel) { return g_core->IsAvailable(size, channel); }
    bool __cdecl Net_ReadP2PPacket(NetWrap *, void *dest, uint32_t cap, uint32_t *size, uint64_t *remote, int channel) {
        return g_core->Read(dest, cap, size, remote, channel);
    }
    bool __cdecl Net_AcceptP2PSessionWithUser(NetWrap *, uint64_t id) { return g_core->Accept(id); }
    bool __cdecl Net_CloseP2PSessionWithUser(NetWrap *, uint64_t id) { return g_core->Close(id); }
    bool __cdecl Net_CloseP2PChannelWithUser(NetWrap *, uint64_t id, int channel) { return g_core->CloseChannel(id, channel); }

#pragma pack(push, 8)
    struct P2PSessionState_t {
        uint8_t m_bConnectionActive;
        uint8_t m_bConnecting;
        uint8_t m_eP2PSessionError;
        uint8_t m_bUsingRelay;
        int32_t m_nBytesQueuedForSend;
        int32_t m_nPacketsQueuedForSend;
        uint32_t m_nRemoteIP;
        uint16_t m_nRemotePort;
    };
#pragma pack(pop)
    static_assert(sizeof(P2PSessionState_t) == 20, "P2PSessionState_t layout");

    bool __cdecl Net_GetP2PSessionState(NetWrap *, uint64_t id, P2PSessionState_t *out) {
        steamproxy::SessionState s;
        const bool ok = g_core->GetSessionState(id, &s);
        if (out) {
            std::memset(out, 0, sizeof(*out));
            out->m_bConnectionActive = s.active;
            out->m_bConnecting = s.connecting;
            out->m_nRemoteIP = s.remoteIp;
            out->m_nRemotePort = s.remotePort;
        }
        return ok;
    }

    bool __cdecl Net_AllowP2PPacketRelay(NetWrap *w, bool allow) {
        using F = bool (*)(void *, bool);
        return ((F)RealVt(w)[7])(w->real, allow);
    }

    // Socket API (index 8..21) is not used for P2P: those slots point at the asm thunks in
    // thunks.S, which forward to the real object untouched.

    void BuildNetVtable() {
        g_netVtbl[0] = (void *)&Net_SendP2PPacket;
        g_netVtbl[1] = (void *)&Net_IsP2PPacketAvailable;
        g_netVtbl[2] = (void *)&Net_ReadP2PPacket;
        g_netVtbl[3] = (void *)&Net_AcceptP2PSessionWithUser;
        g_netVtbl[4] = (void *)&Net_CloseP2PSessionWithUser;
        g_netVtbl[5] = (void *)&Net_CloseP2PChannelWithUser;
        g_netVtbl[6] = (void *)&Net_GetP2PSessionState;
        g_netVtbl[7] = (void *)&Net_AllowP2PPacketRelay;
        for (int i = 8; i < kNetVtableSize; i++) g_netVtbl[i] = sp_fwd_table[i];
    }

    // ---------------- ISteamUser wrapper (GetSteamID only) ----------------
    struct UserWrap {
        void **vtbl;
        void *real;
    };
    void *g_userVtbl[kUserVtableCopy];
    std::map<void *, UserWrap *> g_userWraps;

    uint64_t *__cdecl User_GetSteamID(UserWrap *, uint64_t *ret) {
        *ret = g_fakeId;
        return ret;
    }

    uint64_t RealSteamId() {
        using GetUser = void *(*)();
        using FlatGetId = uint64_t (*)(void *);
        static GetUser realUser = Real<GetUser>("SteamUser");
        static FlatGetId flatGetId = Real<FlatGetId>("SteamAPI_ISteamUser_GetSteamID");
        void *u = realUser ? realUser() : nullptr;
        const uint64_t id = (u && flatGetId) ? flatGetId(u) : 0;
        Log("real SteamID %llu", (unsigned long long)id);
        return id;
    }
} // namespace

// ---------------- exports ----------------
extern "C" void *__cdecl SteamNetworking() {
    using F = void *(*)();
    static F real = Real<F>("SteamNetworking");
    void *r = real ? real() : nullptr;
    Init();
    if (!g_enabled || r == nullptr) return r;
    std::lock_guard<std::mutex> g(g_wrapMu);
    auto &w = g_netWraps[r];
    if (!w) {
        w = new NetWrap{g_netVtbl, r};
        Log("SteamNetworking(): real %p wrapped as %p", r, (void *)w);
    }
    return w;
}

extern "C" void *__cdecl SteamUser() {
    using F = void *(*)();
    static F real = Real<F>("SteamUser");
    void *r = real ? real() : nullptr;
    Init();
    if (!g_enabled || g_fakeId == 0 || r == nullptr) return r;
    std::lock_guard<std::mutex> g(g_wrapMu);
    auto &w = g_userWraps[r];
    if (!w) {
        w = new UserWrap{g_userVtbl, r};
        Log("SteamUser(): real %p wrapped as %p (GetSteamID -> %llu)", r, (void *)w, (unsigned long long)g_fakeId);
    }
    return w;
}

extern "C" void __cdecl SteamAPI_RegisterCallback(void *cb, int id) {
    using F = void (*)(void *, int);
    static F real = Real<F>("SteamAPI_RegisterCallback");
    if (id == kP2PSessionRequest) {
        std::lock_guard<std::mutex> g(g_cbMu);
        g_sessionCallbacks.push_back(cb);
        Log("RegisterCallback(%p, 1202 P2PSessionRequest_t)", cb);
    }
    if (real) real(cb, id);
}

extern "C" void __cdecl SteamAPI_UnregisterCallback(void *cb) {
    using F = void (*)(void *);
    static F real = Real<F>("SteamAPI_UnregisterCallback");
    {
        std::lock_guard<std::mutex> g(g_cbMu);
        for (auto it = g_sessionCallbacks.begin(); it != g_sessionCallbacks.end();) {
            if (*it == cb) {
                Log("UnregisterCallback(%p) for 1202", cb);
                it = g_sessionCallbacks.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (real) real(cb);
}

extern "C" void __cdecl SteamAPI_RunCallbacks() {
    using F = void (*)();
    static F real = Real<F>("SteamAPI_RunCallbacks");
    if (real) real();
    if (!g_enabled) return;

    static std::mutex runMu;
    std::lock_guard<std::mutex> runLock(runMu);
    for (const auto peer : g_core->TakeSessionRequests()) g_undelivered.push_back(peer);
    if (!g_undelivered.empty()) {
        std::vector<uint64_t> keep;
        for (const auto peer : g_undelivered) {
            if (!DeliverSessionRequest(peer)) keep.push_back(peer);
        }
        g_undelivered.swap(keep);
    }

    static uint32_t ticks = 0;
    if (++ticks % 1800 == 0) { // ~30 s at 60 calls/s
        const auto s = g_core->GetStats();
        Log("stats: sent=%llu sendFailed=%llu received=%llu read=%llu dropped=%llu badHeader=%llu waitingRequests=%zu",
            (unsigned long long)s.sent, (unsigned long long)s.sendFailed, (unsigned long long)s.received, (unsigned long long)s.read,
            (unsigned long long)s.dropped, (unsigned long long)s.badHeader, g_undelivered.size());
    }
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = h;
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_logLock);
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(h, path, MAX_PATH);
        g_dir = path;
        g_dir = g_dir.substr(0, g_dir.find_last_of(L'\\'));
        wchar_t logPath[MAX_PATH + 64];
        swprintf(logPath, MAX_PATH + 64, L"%ls\\steamproxy-%lu.log", g_dir.c_str(), GetCurrentProcessId());
        g_log = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        RawLog("dllmain: log open\r\n");
        void *veh = AddVectoredExceptionHandler(1, &InitCrashLogger);
        Log("steamproxy loading into pid %lu, module base %p", GetCurrentProcessId(), (void *)h);
        BuildNetVtable();
        Log("net vtable built");
        for (int i = 0; i < kUserVtableCopy; i++) g_userVtbl[i] = sp_fwd_table[i];
        g_userVtbl[kUserGetSteamID] = (void *)&User_GetSteamID;
        Log("user vtable built; steamproxy loaded");
        RemoveVectoredExceptionHandler(veh);
    }
    return TRUE;
}
