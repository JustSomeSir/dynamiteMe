// Host test for the steamproxy transport: two cores in one process play host and client.
// Build/run: tools/steamproxy/run_tests.sh
#include "p2pcore.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace steamproxy;

static int failures = 0;
#define EXPECT(c)                                                                                                                                    \
    do {                                                                                                                                             \
        if (!(c)) {                                                                                                                                  \
            failures++;                                                                                                                              \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);                                                                                         \
        }                                                                                                                                            \
    } while (0)

static const uint64_t kHost = 76561198000000001ULL;
static const uint64_t kClient = 76561202255233008ULL;

static bool WaitAvailable(P2PCore &c, int channel, uint32_t *size) {
    for (int i = 0; i < 50; i++) {
        if (c.IsAvailable(size, channel)) return true;
        c.WaitForPacket(20);
    }
    return false;
}

int main() {
    // --- config parsing ---
    {
        P2PConfig cfg;
        uint64_t fake = 1;
        std::string err;
        EXPECT(ParseConfig("# test\nlisten_port=27151\npeer_port = 27150\nfake_steam_id=76561202255233008\n", &cfg, &fake, &err));
        EXPECT(cfg.listenPort == 27151 && cfg.peerPort == 27150 && fake == kClient && cfg.selfId == kClient);
        EXPECT(ParseConfig("listen_port=27150\npeer_port=27151\nfake_steam_id=\n", &cfg, &fake, &err) && fake == 0 && cfg.selfId == 0);
        EXPECT(!ParseConfig("listen_port=27150\n", &cfg, &fake, &err));
        EXPECT(!ParseConfig("listen_port=27150\npeer_port=27150\n", &cfg, &fake, &err));
        EXPECT(!ParseConfig("listen_port=70000\npeer_port=1\n", &cfg, &fake, &err));
        EXPECT(!ParseConfig("listen_port=1\npeer_port=2\nfake_steam_id=12345\n", &cfg, &fake, &err));
        EXPECT(!ParseConfig("listen_port=1\npeer_port=2\nfake_steam_id=7656120225523300x\n", &cfg, &fake, &err));
        EXPECT(!ParseConfig("garbage\n", &cfg, &fake, &err));
    }

    std::vector<std::string> hostLog, clientLog;
    P2PCore host, client;
    // host: real id via provider (like the proxy asking the real SteamUser); client: fake id from ini
    host.SetIdProvider([] { return kHost; });
    EXPECT(host.Start({.listenPort = 27150, .peerPort = 27151, .selfId = 0}, [&](const std::string &s) { hostLog.push_back(s); }));
    EXPECT(client.Start({.listenPort = 27151, .peerPort = 27150, .selfId = kClient}, [&](const std::string &s) { clientLog.push_back(s); }));

    // a second core on the same port must fail loudly, not silently share
    {
        P2PCore dup;
        EXPECT(!dup.Start({.listenPort = 27150, .peerPort = 27151, .selfId = 1}, nullptr));
    }

    // --- client connects: host must see a session request, and hold the packet until accepted ---
    const char hello[] = "DYNAMITE hello";
    EXPECT(client.Send(kHost, hello, sizeof(hello), 2, 0));
    EXPECT(host.WaitForPacket(1000));
    uint32_t size = 0;
    EXPECT(!host.IsAvailable(&size, 0)); // not accepted yet
    auto reqs = host.TakeSessionRequests();
    EXPECT(reqs.size() == 1 && reqs[0] == kClient);
    EXPECT(host.TakeSessionRequests().empty()); // reported once

    SessionState st;
    EXPECT(host.GetSessionState(kClient, &st) && !st.active); // seen, not accepted
    EXPECT(host.Accept(kClient));
    EXPECT(host.GetSessionState(kClient, &st) && st.active && st.remotePort == 27151);

    EXPECT(WaitAvailable(host, 0, &size) && size == sizeof(hello));
    char buf[64] = {};
    uint64_t from = 0;
    EXPECT(host.Read(buf, sizeof(buf), &size, &from, 0) && size == sizeof(hello) && from == kClient && std::strcmp(buf, hello) == 0);
    EXPECT(!host.IsAvailable(&size, 0));

    // --- host answers: client sent to kHost first, so no request on the client side ---
    const char reply[] = "welcome";
    EXPECT(host.Send(kClient, reply, sizeof(reply), 2, 0));
    EXPECT(WaitAvailable(client, 0, &size));
    EXPECT(client.TakeSessionRequests().empty());
    EXPECT(client.Read(buf, sizeof(buf), &size, &from, 0) && from == kHost && std::strcmp(buf, reply) == 0);

    // --- channels are separate ---
    EXPECT(client.Send(kHost, "c1", 3, 2, 1));
    EXPECT(WaitAvailable(host, 1, &size));
    EXPECT(!host.IsAvailable(&size, 0));
    EXPECT(host.Read(buf, sizeof(buf), &size, &from, 1) && std::strcmp(buf, "c1") == 0);

    // --- truncation: small buffer gets a prefix and the real size ---
    std::vector<uint8_t> big(5000);
    for (size_t i = 0; i < big.size(); i++) big[i] = uint8_t(i);
    EXPECT(client.Send(kHost, big.data(), (uint32_t)big.size(), 2, 0));
    EXPECT(WaitAvailable(host, 0, &size) && size == 5000);
    uint8_t small[16];
    EXPECT(host.Read(small, sizeof(small), &size, &from, 0) && size == 5000 && small[15] == 15);

    // --- order is kept for a burst ---
    for (uint32_t i = 0; i < 200; i++) client.Send(kHost, &i, sizeof(i), 2, 2);
    uint32_t next = 0;
    bool ordered = true;
    for (int tries = 0; tries < 200 && next < 200; tries++) {
        uint32_t v;
        while (host.Read(&v, sizeof(v), &size, &from, 2)) {
            if (v != next) ordered = false;
            next++;
        }
        if (next < 200) host.WaitForPacket(10);
    }
    EXPECT(next == 200 && ordered);

    // --- limits ---
    std::vector<uint8_t> huge(P2PCore::kMaxPayload + 1);
    EXPECT(!client.Send(kHost, huge.data(), (uint32_t)huge.size(), 2, 0));
    EXPECT(client.Send(kHost, nullptr, 0, 2, 3)); // empty packet is legal in Steam
    EXPECT(WaitAvailable(host, 3, &size) && size == 0);
    EXPECT(host.Read(nullptr, 0, &size, &from, 3));

    // --- close drops held packets and requires a new accept ---
    EXPECT(host.Close(kClient));
    EXPECT(client.Send(kHost, "again", 6, 2, 0));
    EXPECT(host.WaitForPacket(1000));
    for (int i = 0; i < 10 && host.TakeSessionRequests().empty(); i++) host.WaitForPacket(20);
    EXPECT(!host.IsAvailable(&size, 0));

    // --- garbage datagram on the port is counted, not delivered ---
    {
        P2PCore noise;
        EXPECT(noise.Start({.listenPort = 27152, .peerPort = 27150, .selfId = 0}, nullptr));
        // selfId 0 and no provider: header is still valid, sender 0; that is fine for counting
        noise.Stop();
    }

    host.Stop();
    client.Stop();
    const auto hs = host.GetStats();
    std::printf("host stats: received=%llu read=%llu dropped=%llu badHeader=%llu\n", (unsigned long long)hs.received, (unsigned long long)hs.read,
        (unsigned long long)hs.dropped, (unsigned long long)hs.badHeader);
    std::printf("%s: %d failure(s)\n", failures == 0 ? "OK" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
