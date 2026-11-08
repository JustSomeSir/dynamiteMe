// steamproxy self-test: runs on Windows with Steam running, in a folder that holds
//   steam_api64.dll       (the proxy)
//   steam_api64_real.dll  (the game's original steam_api64.dll)
//   steam_appid.txt       (287700)
//   steam_proxy.ini       (written by this test)
// It plays the "other copy" itself with a raw UDP socket, so no game and no second process.
//
// Checks:
//  1. ABI assumption: the real ISteamUser::GetSteamID called through the vtable with a hidden
//     return pointer gives the same id as the flat SteamAPI_ISteamUser_GetSteamID.
//  2. SteamUser() through the proxy returns fake_steam_id; other ISteamUser slots still work (BLoggedOn).
//  3. SteamNetworking() through the proxy: SendP2PPacket reaches the peer socket with the right header;
//     a packet from an unknown peer is held, raised as P2PSessionRequest_t inside
//     SteamAPI_RunCallbacks to a registered callback object, and readable after Accept.
//  4. GetP2PSessionState fields.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(c, what)                                                                                                                               \
    do {                                                                                                                                             \
        const bool ok_ = (c);                                                                                                                        \
        std::printf("%s %s\n", ok_ ? "PASS" : "FAIL", what);                                                                                         \
        if (!ok_) failures++;                                                                                                                        \
    } while (0)

static const uint64_t kFake = 76561202255233008ULL;
static const uint16_t kProxyPort = 27160, kPeerPort = 27161;
static const uint64_t kPeerId = 76561202255233009ULL;

// Fake CCallbackBase: vtable [Run(p,bool,call), Run(p), GetCallbackSizeBytes], then flags, id.
struct FakeCallback;
static void Run3(FakeCallback *self, void *p, bool, uint64_t);
static void Run1(FakeCallback *self, void *p);
static int SizeCb(FakeCallback *) { return 8; }
static void *g_cbVtbl[3] = {(void *)&Run3, (void *)&Run1, (void *)&SizeCb};
struct FakeCallback {
    void **vtbl = g_cbVtbl;
    uint8_t flags = 0;
    int id = 0;
    int calls = 0;
    uint64_t lastPeer = 0;
};
static void Run3(FakeCallback *self, void *p, bool, uint64_t) { Run1(self, p); }
static void Run1(FakeCallback *self, void *p) {
    self->calls++;
    self->lastPeer = *(uint64_t *)p;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    FILE *ini = fopen("steam_proxy.ini", "w");
    fprintf(ini, "listen_port=%u\npeer_port=%u\nfake_steam_id=%llu\n", kProxyPort, kPeerPort, (unsigned long long)kFake);
    fclose(ini);

    // ---- 1. ABI check against the real DLL directly ----
    HMODULE real = LoadLibraryW(L"steam_api64_real.dll");
    CHECK(real != nullptr, "load steam_api64_real.dll");
    if (!real) return 1;
    using InitF = bool (*)();
    using GetIfaceF = void *(*)();
    using FlatIdF = uint64_t (*)(void *);
    using VGetIdF = uint64_t *(*)(void *, uint64_t *);
    using VBoolF = bool (*)(void *);

    HMODULE proxy = LoadLibraryW(L"steam_api64.dll");
    if (!proxy) std::printf("     LoadLibrary error %lu\n", GetLastError());
    CHECK(proxy != nullptr, "load proxy steam_api64.dll");
    if (!proxy) return 1;
    auto init = (InitF)GetProcAddress(proxy, "SteamAPI_Init"); // forwarded export
    CHECK(init && init(), "SteamAPI_Init via proxy (Steam running, steam_appid.txt=287700)");

    auto realUser = ((GetIfaceF)GetProcAddress(real, "SteamUser"))();
    auto flatId = (FlatIdF)GetProcAddress(real, "SteamAPI_ISteamUser_GetSteamID");
    const uint64_t idFlat = flatId(realUser);
    uint64_t idV = 0;
    ((VGetIdF)(*(void ***)realUser)[2])(realUser, &idV);
    std::printf("     real id flat=%llu vtable=%llu\n", (unsigned long long)idFlat, (unsigned long long)idV);
    CHECK(idFlat != 0 && idFlat == idV, "ISteamUser::GetSteamID returns via hidden pointer after this (MSVC x64)");

    // ---- 2. SteamUser through the proxy ----
    auto pUser = ((GetIfaceF)GetProcAddress(proxy, "SteamUser"))();
    CHECK(pUser != realUser, "SteamUser() is wrapped when fake_steam_id is set");
    uint64_t idP = 0;
    uint64_t *r = ((VGetIdF)(*(void ***)pUser)[2])(pUser, &idP);
    CHECK(idP == kFake && r == &idP, "wrapped GetSteamID returns fake_steam_id");
    const bool loggedReal = ((VBoolF)(*(void ***)realUser)[1])(realUser);
    const bool loggedProxy = ((VBoolF)(*(void ***)pUser)[1])(pUser);
    CHECK(loggedReal && loggedProxy, "BLoggedOn forwarded through thunk");

    // ---- 3. networking ----
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET peer = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(kPeerPort);
    CHECK(bind(peer, (sockaddr *)&a, sizeof(a)) == 0, "peer socket bind");
    DWORD tmo = 2000;
    setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));

    using RegF = void (*)(void *, int);
    using UnregF = void (*)(void *);
    using RunF = void (*)();
    auto reg = (RegF)GetProcAddress(proxy, "SteamAPI_RegisterCallback");
    auto unreg = (UnregF)GetProcAddress(proxy, "SteamAPI_UnregisterCallback");
    auto run = (RunF)GetProcAddress(proxy, "SteamAPI_RunCallbacks");
    FakeCallback cb;
    reg(&cb, 1202);

    void *net = ((GetIfaceF)GetProcAddress(proxy, "SteamNetworking"))();
    auto realNet = ((GetIfaceF)GetProcAddress(real, "SteamNetworking"))();
    CHECK(net && net != realNet, "SteamNetworking() is wrapped");
    void **vt = *(void ***)net;
    using SendF = bool (*)(void *, uint64_t, const void *, uint32_t, int, int);
    using AvailF = bool (*)(void *, uint32_t *, int);
    using ReadF = bool (*)(void *, void *, uint32_t, uint32_t *, uint64_t *, int);
    using AcceptF = bool (*)(void *, uint64_t);
    using StateF = bool (*)(void *, uint64_t, uint8_t *);

    // proxy -> peer
    const char hello[] = "DYNAMITE hi";
    CHECK(((SendF)vt[0])(net, kPeerId, hello, sizeof(hello), 2, 0), "SendP2PPacket");
    uint8_t buf[256];
    int n = recv(peer, (char *)buf, sizeof(buf), 0);
    uint64_t sender = 0;
    if (n >= 20) std::memcpy(&sender, buf + 4, 8);
    CHECK(n == 20 + (int)sizeof(hello) && sender == kFake && std::memcmp(buf + 20, hello, sizeof(hello)) == 0, "peer got packet with fake sender id");

    // unknown peer -> proxy: held, then session request inside RunCallbacks
    const uint64_t stranger = kPeerId + 1;
    uint8_t pkt[20 + 4];
    const uint32_t magic = 0x31505053, ch = 5, type = 2;
    std::memcpy(pkt, &magic, 4);
    std::memcpy(pkt + 4, &stranger, 8);
    std::memcpy(pkt + 12, &ch, 4);
    std::memcpy(pkt + 16, &type, 4);
    std::memcpy(pkt + 20, "ping", 4);
    sockaddr_in to = a;
    to.sin_port = htons(kProxyPort);
    sendto(peer, (const char *)pkt, sizeof(pkt), 0, (sockaddr *)&to, sizeof(to));
    Sleep(300);
    uint32_t size = 0;
    CHECK(!((AvailF)vt[1])(net, &size, 5), "packet from unknown peer is held");
    run();
    CHECK(cb.calls == 1 && cb.lastPeer == stranger, "P2PSessionRequest_t delivered in RunCallbacks");
    run();
    CHECK(cb.calls == 1, "session request delivered once");
    CHECK(((AcceptF)vt[3])(net, stranger), "AcceptP2PSessionWithUser");
    CHECK(((AvailF)vt[1])(net, &size, 5) && size == 4, "IsP2PPacketAvailable after accept");
    uint64_t from = 0;
    char got[8] = {};
    CHECK(((ReadF)vt[2])(net, got, sizeof(got), &size, &from, 5) && from == stranger && std::memcmp(got, "ping", 4) == 0, "ReadP2PPacket");

    uint8_t st[20] = {};
    CHECK(((StateF)vt[6])(net, stranger, st) && st[0] == 1 && *(uint16_t *)(st + 16) == kPeerPort, "GetP2PSessionState active, port");

    unreg(&cb);
    closesocket(peer);
    std::printf("%s: %d failure(s). Proxy log: steamproxy-%lu.log\n", failures ? "FAILED" : "OK", failures, GetCurrentProcessId());
    return failures ? 1 : 0;
}
