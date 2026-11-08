// Host-side test for InstanceName.h (second game copy on one PC).
#include "InstanceName.h"

#include <cstdio>
#include <string>

static int failures = 0;
#define EXPECT(cond)                                                                                                                                 \
    do {                                                                                                                                             \
        if (!(cond)) {                                                                                                                               \
            failures++;                                                                                                                              \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                                                      \
        }                                                                                                                                            \
    } while (0)

int main() {
    using namespace Dynamite;
    std::wstring st;

    // no id: the game's call is left alone (normal play)
    EXPECT(InstanceMutexName(kGameMutexName, L"", st) == nullptr);
    // other mutexes are never renamed (Steam overlay creates many)
    EXPECT(InstanceMutexName(L"SteamXInput_mutex-IPCWrapper", L"2", st) == nullptr);
    EXPECT(InstanceMutexName(nullptr, L"2", st) == nullptr);
    // the game mutex gets a per-instance name, different per id
    const wchar_t *a = InstanceMutexName(kGameMutexName, L"2", st);
    EXPECT(a != nullptr && std::wstring(a) == L"METAL GEAR SOLID V: THE PHANTOM PAIN #2");
    std::wstring st2;
    const wchar_t *b = InstanceMutexName(kGameMutexName, L"3", st2);
    EXPECT(b != nullptr && std::wstring(a) != std::wstring(b));
    // near-misses are not the game mutex
    EXPECT(InstanceMutexName(L"METAL GEAR SOLID V: THE PHANTOM PAIN ", L"2", st) == nullptr);

    // ids are sanitized: no namespace or path tricks, stops at end of line, capped length
    EXPECT(SanitizeInstanceId(L"2") == L"2");
    EXPECT(SanitizeInstanceId(L"client-1\r\n") == L"client-1");
    EXPECT(SanitizeInstanceId(L"Global\\x") == L"Globalx");
    EXPECT(SanitizeInstanceId(L"..\\..\\a") == L"a");
    EXPECT(SanitizeInstanceId(L"   ") == L"");
    EXPECT(SanitizeInstanceId(L"\r\n2") == L"");
    EXPECT(SanitizeInstanceId(L"abcdefghijklmnopqrstuvwxyz").size() == 16);

    std::printf("%s: %d failure(s)\n", failures == 0 ? "OK" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
