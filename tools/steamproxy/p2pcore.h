#ifndef STEAMPROXY_P2PCORE_H
#define STEAMPROXY_P2PCORE_H

// Portable core of the test transport: Steam-P2P-like messaging between two local processes over
// localhost UDP. No Steam and no Windows types here, so it is unit-tested on Linux
// (test_p2pcore.cpp); proxy.cpp maps ISteamNetworking005 calls onto it.
//
// Semantics copied from ISteamNetworking (SDK 1.3x) where the game can notice them:
// - packets are per channel; ReadP2PPacket truncates to the caller's buffer and reports the real size;
// - packets from a peer we never sent to and never accepted are held, and the peer is reported once
//   as a session request (P2PSessionRequest_t) until AcceptP2PSessionWithUser;
// - sending to a peer implicitly accepts that peer;
// - CloseP2PSessionWithUser drops the peer's held packets and its accepted state.

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace steamproxy {

    struct P2PConfig {
        uint16_t listenPort = 0; // this copy receives here (127.0.0.1)
        uint16_t peerPort = 0;   // the other copy's listenPort
        uint64_t selfId = 0;     // SteamID written into every packet we send (0 = ask idProvider)
    };

    struct SessionState {
        bool active = false;
        bool connecting = false;
        uint32_t remoteIp = 0; // host byte order
        uint16_t remotePort = 0;
    };

    struct Stats {
        uint64_t sent = 0, sendFailed = 0, received = 0, dropped = 0, read = 0, badHeader = 0;
    };

    class P2PCore {
      public:
        static constexpr uint32_t kMagic = 0x31505053; // "SPP1" little endian
        static constexpr size_t kHeaderSize = 4 + 8 + 4 + 4;
        static constexpr size_t kMaxPayload = 65000; // one UDP datagram on loopback
        static constexpr size_t kMaxQueued = 4096;   // per process, oldest dropped beyond this

        using Logger = std::function<void(const std::string &)>;

        P2PCore() = default;
        ~P2PCore();
        P2PCore(const P2PCore &) = delete;
        P2PCore &operator=(const P2PCore &) = delete;

        // Binds listenPort and starts the receive thread. False on socket errors (see log).
        bool Start(const P2PConfig &cfg, Logger log);
        void Stop();
        bool Running() const { return running_; }

        // Lazily supplies selfId when cfg.selfId == 0 (proxy: the real SteamUser()->GetSteamID()).
        void SetIdProvider(std::function<uint64_t()> p) { idProvider_ = std::move(p); }
        uint64_t SelfId();

        bool Send(uint64_t target, const void *data, uint32_t size, int sendType, int channel);
        bool IsAvailable(uint32_t *size, int channel);
        bool Read(void *dest, uint32_t cap, uint32_t *size, uint64_t *sender, int channel);
        bool Accept(uint64_t peer);
        bool Close(uint64_t peer);
        bool CloseChannel(uint64_t peer, int channel);
        bool GetSessionState(uint64_t peer, SessionState *out);

        // Peers that sent us something before we accepted them; each is returned once.
        std::vector<uint64_t> TakeSessionRequests();

        // Blocks until a packet arrives or timeoutMs passes (tests only).
        bool WaitForPacket(int timeoutMs);

        Stats GetStats();

      private:
        struct Packet {
            uint64_t sender;
            int channel;
            std::vector<uint8_t> data;
        };

        void RecvLoop();
        bool IsAcceptedLocked(uint64_t peer) const { return accepted_.count(peer) != 0; }
        void LogLine(const std::string &s) {
            std::lock_guard<std::mutex> g(logMu_); // called from the game thread and the receive thread
            if (log_) log_(s);
        }

        P2PConfig cfg_{};
        Logger log_;
        std::function<uint64_t()> idProvider_;
        uint64_t selfIdCache_ = 0;

        intptr_t sock_ = -1;
        bool running_ = false;
        bool stopping_ = false;
        std::thread thread_;

        std::mutex mu_;
        std::mutex logMu_; // never taken before mu_ (LogLine may run while mu_ is held)
        std::condition_variable cv_;
        std::deque<Packet> queue_;
        std::set<uint64_t> accepted_;
        std::set<uint64_t> requested_; // reported as session request, not yet accepted
        std::vector<uint64_t> pendingRequests_;
        std::set<uint64_t> seen_; // peers we have received from
        Stats stats_{};
        int loggedPackets_ = 0;
    };

    // "steam_proxy.ini" parser: key=value lines, '#' or ';' comments. Unknown keys are ignored.
    bool ParseConfig(const std::string &text, P2PConfig *cfg, uint64_t *fakeSelfId, std::string *error);

} // namespace steamproxy

#endif
