#include "SecondInstance.h"

#include "InstanceName.h"
#include "MinHook.h"
#include "spdlog/spdlog.h"
#include "windows.h"

#include <fstream>
#include <string>

namespace Dynamite {
    namespace {
        std::wstring g_instanceId;
        decltype(&CreateMutexW) CreateMutexW_Orig = nullptr;

        HANDLE WINAPI CreateMutexWHook(LPSECURITY_ATTRIBUTES sa, BOOL initialOwner, LPCWSTR name) {
            std::wstring storage;
            if (const auto renamed = InstanceMutexName(name, g_instanceId, storage); renamed != nullptr) {
                HANDLE h = CreateMutexW_Orig(sa, initialOwner, renamed);
                const DWORD err = GetLastError();
                spdlog::info("second instance: game mutex renamed to instance {}, lastError={}", std::string(g_instanceId.begin(), g_instanceId.end()), err);
                SetLastError(err);
                return h;
            }
            return CreateMutexW_Orig(sa, initialOwner, name);
        }

        std::wstring ReadInstanceId() {
            wchar_t env[64]{};
            if (GetEnvironmentVariableW(L"DYNAMITE_INSTANCE", env, 64) > 0) {
                return SanitizeInstanceId(env);
            }
            std::wifstream f("dynamite/instance.txt");
            std::wstring line;
            if (f && std::getline(f, line)) {
                return SanitizeInstanceId(line);
            }
            return {};
        }
    } // namespace

    std::string InstanceIdUtf8() {
        return {g_instanceId.begin(), g_instanceId.end()}; // SanitizeInstanceId keeps ASCII only
    }

    void SetupTestTransport() {
        // Test-only: tools/steamproxy in hook mode. Present only in test copies, next to mgsvtpp.exe.
        if (GetFileAttributesW(L"steam_proxy.ini") == INVALID_FILE_ATTRIBUTES) {
            return;
        }
        HMODULE shim = LoadLibraryW(L"steamshim.dll");
        spdlog::info("test transport: steam_proxy.ini found, steamshim.dll {} (error {})", shim ? "loaded" : "NOT loaded", shim ? 0 : GetLastError());
    }

    void SetupSecondInstance() {
        g_instanceId = ReadInstanceId();
        if (g_instanceId.empty()) {
            return; // normal play: nothing hooked
        }

        const auto s = MH_Initialize();
        if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
            spdlog::error("second instance: MH_Initialize failed: {}", MH_StatusToString(s));
            return;
        }

        auto *target = reinterpret_cast<LPVOID>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateMutexW"));
        auto st = MH_CreateHook(target, reinterpret_cast<LPVOID>(&CreateMutexWHook), reinterpret_cast<LPVOID *>(&CreateMutexW_Orig));
        if (st == MH_OK) {
            st = MH_EnableHook(target);
        }
        spdlog::info("second instance: id {}, CreateMutexW hook {}", std::string(g_instanceId.begin(), g_instanceId.end()), MH_StatusToString(st));
    }
} // namespace Dynamite
