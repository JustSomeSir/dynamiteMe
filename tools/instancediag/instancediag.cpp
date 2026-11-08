// instancediag: a diagnostic dinput8.dll for a *test copy* of the game.
//
// Question it answers: why does a second mgsvtpp.exe vanish without a message while one copy runs?
// It logs, per process, every named mutex/event the game creates or opens (and whether the name
// already existed), any process the game starts (DRM/Steam relaunch), and how the process exits.
// It changes nothing: every hook calls the original and returns its result unchanged.
//
// Log: <game dir>\instancediag-<pid>.log, one line per event, flushed immediately.
// Put it only in a test copy, next to mgsvtpp.exe. Remove it to go back to vanilla.

#include <windows.h>

#include <MinHook.h>
#include <cstdarg>
#include <cstdio>
#include <cwchar>

namespace {
    HMODULE g_self = nullptr;
    HANDLE g_log = INVALID_HANDLE_VALUE;
    CRITICAL_SECTION g_lock;
    LARGE_INTEGER g_freq{}, g_t0{};

    void Log(const wchar_t *fmt, ...) {
        if (g_log == INVALID_HANDLE_VALUE) return;
        const DWORD savedErr = GetLastError(); // never disturb the caller's GetLastError
        wchar_t wbuf[1200];
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double ms = 1000.0 * double(now.QuadPart - g_t0.QuadPart) / double(g_freq.QuadPart);
        int n = swprintf(wbuf, 64, L"%10.1f ms tid=%lu ", ms, GetCurrentThreadId());
        va_list ap;
        va_start(ap, fmt);
        n += vswprintf(wbuf + n, 1100, fmt, ap);
        va_end(ap);
        wbuf[n++] = L'\r';
        wbuf[n++] = L'\n';
        char buf[2400];
        const int len = WideCharToMultiByte(CP_UTF8, 0, wbuf, n, buf, sizeof(buf), nullptr, nullptr);
        EnterCriticalSection(&g_lock);
        DWORD written = 0;
        WriteFile(g_log, buf, len, &written, nullptr);
        LeaveCriticalSection(&g_lock);
        SetLastError(savedErr);
    }

    const wchar_t *Str(const wchar_t *s) { return s ? s : L"(null)"; }

    // Caller's return address -> module name, so we can tell game code from Steam/DRM code.
    void CallerModule(void *ret, wchar_t *out, size_t outLen) {
        HMODULE m = nullptr;
        out[0] = 0;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)ret, &m)) {
            wchar_t path[MAX_PATH];
            GetModuleFileNameW(m, path, MAX_PATH);
            const wchar_t *base = wcsrchr(path, L'\\');
            wcsncpy(out, base ? base + 1 : path, outLen - 1);
            out[outLen - 1] = 0;
        } else {
            wcsncpy(out, L"?", outLen);
        }
    }

#define CALLER()                                                                                                                                     \
    wchar_t caller[64];                                                                                                                              \
    CallerModule(__builtin_return_address(0), caller, 64)

    // --- originals ---
    decltype(&CreateMutexW) oCreateMutexW;
    decltype(&CreateMutexA) oCreateMutexA;
    decltype(&CreateMutexExW) oCreateMutexExW;
    decltype(&OpenMutexW) oOpenMutexW;
    decltype(&CreateEventW) oCreateEventW;
    decltype(&CreateEventA) oCreateEventA;
    decltype(&CreateProcessW) oCreateProcessW;
    decltype(&CreateProcessA) oCreateProcessA;
    decltype(&ExitProcess) oExitProcess;
    decltype(&TerminateProcess) oTerminateProcess;
    decltype(&MessageBoxW) oMessageBoxW;
    decltype(&MessageBoxA) oMessageBoxA;

    void LogNamed(const wchar_t *api, const wchar_t *name, HANDLE h, const wchar_t *caller) {
        const DWORD err = GetLastError();
        Log(L"%ls name=\"%ls\" handle=%p lastError=%lu%ls caller=%ls", api, Str(name), h, err, err == ERROR_ALREADY_EXISTS ? L" (ALREADY_EXISTS)" : L"",
            caller);
        SetLastError(err);
    }

    HANDLE WINAPI hCreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL own, LPCWSTR name) {
        CALLER();
        HANDLE h = oCreateMutexW(sa, own, name);
        if (name) LogNamed(L"CreateMutexW", name, h, caller);
        return h;
    }

    HANDLE WINAPI hCreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL own, LPCSTR name) {
        CALLER();
        HANDLE h = oCreateMutexA(sa, own, name);
        if (name) {
            wchar_t w[512];
            MultiByteToWideChar(CP_ACP, 0, name, -1, w, 512);
            LogNamed(L"CreateMutexA", w, h, caller);
        }
        return h;
    }

    HANDLE WINAPI hCreateMutexExW(LPSECURITY_ATTRIBUTES sa, LPCWSTR name, DWORD flags, DWORD access) {
        CALLER();
        HANDLE h = oCreateMutexExW(sa, name, flags, access);
        if (name) LogNamed(L"CreateMutexExW", name, h, caller);
        return h;
    }

    HANDLE WINAPI hOpenMutexW(DWORD access, BOOL inherit, LPCWSTR name) {
        CALLER();
        HANDLE h = oOpenMutexW(access, inherit, name);
        LogNamed(L"OpenMutexW", name, h, caller);
        return h;
    }

    HANDLE WINAPI hCreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCWSTR name) {
        CALLER();
        HANDLE h = oCreateEventW(sa, manual, initial, name);
        if (name) LogNamed(L"CreateEventW", name, h, caller);
        return h;
    }

    HANDLE WINAPI hCreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCSTR name) {
        CALLER();
        HANDLE h = oCreateEventA(sa, manual, initial, name);
        if (name) {
            wchar_t w[512];
            MultiByteToWideChar(CP_ACP, 0, name, -1, w, 512);
            LogNamed(L"CreateEventA", w, h, caller);
        }
        return h;
    }

    BOOL WINAPI hCreateProcessW(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags, LPVOID env,
        LPCWSTR dir, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi) {
        CALLER();
        Log(L"CreateProcessW app=\"%ls\" cmd=\"%ls\" caller=%ls", Str(app), Str(cmd), caller);
        return oCreateProcessW(app, cmd, pa, ta, inherit, flags, env, dir, si, pi);
    }

    BOOL WINAPI hCreateProcessA(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags, LPVOID env,
        LPCSTR dir, LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi) {
        CALLER();
        Log(L"CreateProcessA app=\"%hs\" cmd=\"%hs\" caller=%ls", app ? app : "(null)", cmd ? cmd : "(null)", caller);
        return oCreateProcessA(app, cmd, pa, ta, inherit, flags, env, dir, si, pi);
    }

    void WINAPI hExitProcess(UINT code) {
        CALLER();
        Log(L"ExitProcess code=%u caller=%ls", code, caller);
        oExitProcess(code);
    }

    BOOL WINAPI hTerminateProcess(HANDLE p, UINT code) {
        CALLER();
        if (p == GetCurrentProcess() || GetProcessId(p) == GetCurrentProcessId()) {
            Log(L"TerminateProcess(self) code=%u caller=%ls", code, caller);
        }
        return oTerminateProcess(p, code);
    }

    int WINAPI hMessageBoxW(HWND w, LPCWSTR text, LPCWSTR title, UINT type) {
        CALLER();
        Log(L"MessageBoxW title=\"%ls\" text=\"%ls\" caller=%ls", Str(title), Str(text), caller);
        return oMessageBoxW(w, text, title, type);
    }

    int WINAPI hMessageBoxA(HWND w, LPCSTR text, LPCSTR title, UINT type) {
        CALLER();
        Log(L"MessageBoxA title=\"%hs\" text=\"%hs\" caller=%ls", title ? title : "(null)", text ? text : "(null)", caller);
        return oMessageBoxA(w, text, title, type);
    }

    template <typename D, typename T> void Hook(const wchar_t *dll, const char *fn, D detour, T *orig) {
        HMODULE m = GetModuleHandleW(dll);
        void *target = m ? (void *)GetProcAddress(m, fn) : nullptr;
        if (!target) {
            Log(L"hook %hs: not found", fn);
            return;
        }
        MH_STATUS s = MH_CreateHook(target, (LPVOID)detour, (LPVOID *)orig);
        if (s == MH_OK) s = MH_EnableHook(target);
        Log(L"hook %ls!%hs: %hs", dll, fn, MH_StatusToString(s));
    }

    void Start() {
        InitializeCriticalSection(&g_lock);
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_t0);

        wchar_t dir[MAX_PATH];
        GetModuleFileNameW(g_self, dir, MAX_PATH);
        if (wchar_t *slash = wcsrchr(dir, L'\\')) *slash = 0;
        wchar_t path[MAX_PATH + 64];
        swprintf(path, MAX_PATH + 64, L"%ls\\instancediag-%lu.log", dir, GetCurrentProcessId());
        g_log = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        SYSTEMTIME st;
        GetLocalTime(&st);
        Log(L"start pid=%lu parent-cmd=\"%ls\" exe=\"%ls\" local=%04u-%02u-%02u %02u:%02u:%02u", GetCurrentProcessId(), GetCommandLineW(), exe,
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

        if (MH_Initialize() != MH_OK) {
            Log(L"MH_Initialize failed");
            return;
        }
        Hook(L"kernel32.dll", "CreateMutexW", &hCreateMutexW, &oCreateMutexW);
        Hook(L"kernel32.dll", "CreateMutexA", &hCreateMutexA, &oCreateMutexA);
        Hook(L"kernel32.dll", "CreateMutexExW", &hCreateMutexExW, &oCreateMutexExW);
        Hook(L"kernel32.dll", "OpenMutexW", &hOpenMutexW, &oOpenMutexW);
        Hook(L"kernel32.dll", "CreateEventW", &hCreateEventW, &oCreateEventW);
        Hook(L"kernel32.dll", "CreateEventA", &hCreateEventA, &oCreateEventA);
        Hook(L"kernel32.dll", "CreateProcessW", &hCreateProcessW, &oCreateProcessW);
        Hook(L"kernel32.dll", "CreateProcessA", &hCreateProcessA, &oCreateProcessA);
        Hook(L"kernel32.dll", "ExitProcess", &hExitProcess, &oExitProcess);
        Hook(L"kernel32.dll", "TerminateProcess", &hTerminateProcess, &oTerminateProcess);
        LoadLibraryW(L"user32.dll"); // already a dependency of the game; makes sure it is mapped
        Hook(L"user32.dll", "MessageBoxW", &hMessageBoxW, &oMessageBoxW);
        Hook(L"user32.dll", "MessageBoxA", &hMessageBoxA, &oMessageBoxA);
    }

    // --- dinput8 proxy: forward the one export the game uses to the system DLL ---
    using DirectInput8Create_t = HRESULT(WINAPI *)(HINSTANCE, DWORD, REFIID, LPVOID *, void *);
    DirectInput8Create_t g_realCreate = nullptr;
} // namespace

extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD ver, REFIID riid, LPVOID *out, void *outer) {
    if (!g_realCreate) {
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        wcscat(sys, L"\\dinput8.dll");
        HMODULE real = LoadLibraryW(sys);
        g_realCreate = real ? (DirectInput8Create_t)GetProcAddress(real, "DirectInput8Create") : nullptr;
        Log(L"DirectInput8Create forwarded to system dinput8: %ls", g_realCreate ? L"ok" : L"FAILED");
        if (!g_realCreate) return E_FAIL;
    }
    return g_realCreate(hinst, ver, riid, out, outer);
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
        Start();
    }
    return TRUE;
}
