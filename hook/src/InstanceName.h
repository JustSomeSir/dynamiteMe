#ifndef HOOK_INSTANCENAME_H
#define HOOK_INSTANCENAME_H

// Test-only: lets two copies of the game run on one PC.
//
// mgsvtpp.exe creates the named mutex "METAL GEAR SOLID V: THE PHANTOM PAIN" about 1 s after start
// and exits with code 0 if it already exists (seen with tools/instancediag on 1.0.15.4). When a copy
// is given an instance id, its mutex gets a per-instance name, so each copy only sees itself.
//
// The id comes from the env var DYNAMITE_INSTANCE, else from the first line of
// <game dir>/dynamite/instance.txt. No id (the normal case for players) = no change at all.

#include <string>

namespace Dynamite {

    inline constexpr const wchar_t *kGameMutexName = L"METAL GEAR SOLID V: THE PHANTOM PAIN";

    // Keeps letters, digits, '-' and '_' (max 16), so an id can't add path or namespace parts
    // ("Global\\", "..\\") to a kernel object name. Empty result = no instance id.
    inline std::wstring SanitizeInstanceId(const std::wstring &raw) {
        std::wstring out;
        for (const wchar_t c : raw) {
            if (out.size() >= 16) break;
            const bool ok = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'-' || c == L'_';
            if (ok) out.push_back(c);
            else if (c == L'\r' || c == L'\n') break;
        }
        return out;
    }

    // The name to create instead of `name`, or nullptr to keep the original call unchanged.
    // `storage` holds the new name; it must outlive the CreateMutexW call.
    inline const wchar_t *InstanceMutexName(const wchar_t *name, const std::wstring &instanceId, std::wstring &storage) {
        if (name == nullptr || instanceId.empty()) return nullptr;
        if (std::wstring(name) != kGameMutexName) return nullptr;
        storage = std::wstring(kGameMutexName) + L" #" + instanceId;
        return storage.c_str();
    }

} // namespace Dynamite

#endif // HOOK_INSTANCENAME_H
