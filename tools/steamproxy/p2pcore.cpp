#include "p2pcore.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#define SP_CLOSESOCKET closesocket
#define SP_INVALID(s) ((SOCKET)(s) == INVALID_SOCKET)
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#define SP_CLOSESOCKET close
#define SP_INVALID(s) ((s) < 0)
#endif

namespace steamproxy {

    namespace {
        void Put32(uint8_t *p, uint32_t v) { std::memcpy(p, &v, 4); }
        void Put64(uint8_t *p, uint64_t v) { std::memcpy(p, &v, 8); }
        uint32_t Get32(const uint8_t *p) {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }
        uint64_t Get64(const uint8_t *p) {
            uint64_t v;
            std::memcpy(&v, p, 8);
            return v;
        }

        sockaddr_in Loopback(uint16_t port) {
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(port);
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            return a;
        }

        std::string Trim(const std::string &s) {
            const auto b = s.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) return "";
            const auto e = s.find_last_not_of(" \t\r\n");
            return s.substr(b, e - b + 1);
        }

        bool ParseU64(const std::string &s, uint64_t *out) {
            if (s.empty() || s.size() > 20) return false;
            uint64_t v = 0;
            for (const char c : s) {
                if (c < '0' || c > '9') return false;
                const uint64_t d = uint64_t(c - '0');
                if (v > (UINT64_MAX - d) / 10) return false;
                v = v * 10 + d;
            }
            *out = v;
            return true;
        }
    } // namespace

    P2PCore::~P2PCore() { Stop(); }

    bool P2PCore::Start(const P2PConfig &cfg, Logger log) {
        cfg_ = cfg;
        log_ = std::move(log);
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            LogLine("WSAStartup failed");
            return false;
        }
#endif
        const auto s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (SP_INVALID(s)) {
            LogLine("socket() failed");
            return false;
        }
        sock_ = (intptr_t)s;

        // Big buffers: the game can burst reliable packets during mission load.
        int buf = 4 * 1024 * 1024;
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&buf, sizeof(buf));
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&buf, sizeof(buf));
#ifdef _WIN32
        DWORD tmo = 100;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
#else
        timeval tmo{0, 100 * 1000};
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo));
#endif
        auto addr = Loopback(cfg_.listenPort);
        if (bind(s, (sockaddr *)&addr, sizeof(addr)) != 0) {
            LogLine("bind 127.0.0.1:" + std::to_string(cfg_.listenPort) + " failed (port in use? another copy with the same ini?)");
            SP_CLOSESOCKET(s);
            sock_ = -1;
            return false;
        }

        running_ = true;
        stopping_ = false;
        thread_ = std::thread([this] { RecvLoop(); });
        LogLine("p2p core up: listen 127.0.0.1:" + std::to_string(cfg_.listenPort) + ", peer 127.0.0.1:" + std::to_string(cfg_.peerPort));
        return true;
    }

    void P2PCore::Stop() {
        if (!running_) return;
        {
            std::lock_guard<std::mutex> g(mu_);
            stopping_ = true;
        }
        if (thread_.joinable()) thread_.join();
        SP_CLOSESOCKET(sock_);
        sock_ = -1;
        running_ = false;
        cv_.notify_all();
    }

    uint64_t P2PCore::SelfId() {
        if (cfg_.selfId != 0) return cfg_.selfId;
        if (selfIdCache_ == 0 && idProvider_) selfIdCache_ = idProvider_();
        return selfIdCache_;
    }

    bool P2PCore::Send(uint64_t target, const void *data, uint32_t size, int sendType, int channel) {
        if (!running_ || (data == nullptr && size > 0)) return false;
        if (size > kMaxPayload) {
            std::lock_guard<std::mutex> g(mu_);
            stats_.sendFailed++;
            LogLine("send: payload " + std::to_string(size) + " bytes > " + std::to_string(kMaxPayload) + ", dropped");
            return false;
        }

        std::vector<uint8_t> pkt(kHeaderSize + size);
        Put32(pkt.data(), kMagic);
        Put64(pkt.data() + 4, SelfId());
        Put32(pkt.data() + 12, uint32_t(channel));
        Put32(pkt.data() + 16, uint32_t(sendType));
        if (size) std::memcpy(pkt.data() + kHeaderSize, data, size);

        const auto addr = Loopback(cfg_.peerPort);
        const auto n = sendto((decltype(socket(0, 0, 0)))sock_, (const char *)pkt.data(), (int)pkt.size(), 0, (const sockaddr *)&addr, sizeof(addr));

        std::lock_guard<std::mutex> g(mu_);
        accepted_.insert(target); // Steam: sending to a user accepts their session
        requested_.erase(target);
        if (n < 0 || (size_t)n != pkt.size()) {
            stats_.sendFailed++;
            return false;
        }
        stats_.sent++;
        return true;
    }

    void P2PCore::RecvLoop() {
        std::vector<uint8_t> buf(kHeaderSize + kMaxPayload + 16);
        for (;;) {
            {
                std::lock_guard<std::mutex> g(mu_);
                if (stopping_) return;
            }
            sockaddr_in from{};
            socklen_t fromLen = sizeof(from);
            const auto n = recvfrom((decltype(socket(0, 0, 0)))sock_, (char *)buf.data(), (int)buf.size(), 0, (sockaddr *)&from, &fromLen);
            if (n <= 0) continue; // timeout or transient error

            std::lock_guard<std::mutex> g(mu_);
            if ((size_t)n < kHeaderSize || Get32(buf.data()) != kMagic || from.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
                stats_.badHeader++;
                continue;
            }
            Packet p;
            p.sender = Get64(buf.data() + 4);
            p.channel = (int)Get32(buf.data() + 12);
            p.data.assign(buf.begin() + kHeaderSize, buf.begin() + n);
            stats_.received++;
            seen_.insert(p.sender);

            if (!IsAcceptedLocked(p.sender) && requested_.insert(p.sender).second) {
                pendingRequests_.push_back(p.sender);
                LogLine("session request from " + std::to_string(p.sender));
            }
            if (loggedPackets_ < 20) {
                loggedPackets_++;
                LogLine("recv from " + std::to_string(p.sender) + " ch " + std::to_string(p.channel) + " " + std::to_string(p.data.size()) + " bytes");
            }
            if (queue_.size() >= kMaxQueued) {
                queue_.pop_front();
                stats_.dropped++;
            }
            queue_.push_back(std::move(p));
            cv_.notify_all();
        }
    }

    bool P2PCore::IsAvailable(uint32_t *size, int channel) {
        std::lock_guard<std::mutex> g(mu_);
        for (const auto &p : queue_) {
            if (p.channel == channel && IsAcceptedLocked(p.sender)) {
                if (size) *size = (uint32_t)p.data.size();
                return true;
            }
        }
        return false;
    }

    bool P2PCore::Read(void *dest, uint32_t cap, uint32_t *size, uint64_t *sender, int channel) {
        std::lock_guard<std::mutex> g(mu_);
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
            if (it->channel != channel || !IsAcceptedLocked(it->sender)) continue;
            const uint32_t n = std::min<uint32_t>(cap, (uint32_t)it->data.size());
            if (n && dest) std::memcpy(dest, it->data.data(), n);
            if (size) *size = (uint32_t)it->data.size();
            if (sender) *sender = it->sender;
            queue_.erase(it);
            stats_.read++;
            return true;
        }
        return false;
    }

    bool P2PCore::Accept(uint64_t peer) {
        std::lock_guard<std::mutex> g(mu_);
        accepted_.insert(peer);
        requested_.erase(peer);
        LogLine("accept " + std::to_string(peer));
        return true;
    }

    bool P2PCore::Close(uint64_t peer) {
        std::lock_guard<std::mutex> g(mu_);
        const bool known = accepted_.erase(peer) + requested_.erase(peer) > 0;
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const Packet &p) { return p.sender == peer; }), queue_.end());
        LogLine("close session " + std::to_string(peer));
        return known;
    }

    bool P2PCore::CloseChannel(uint64_t peer, int channel) {
        std::lock_guard<std::mutex> g(mu_);
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const Packet &p) { return p.sender == peer && p.channel == channel; }), queue_.end());
        return accepted_.count(peer) != 0;
    }

    bool P2PCore::GetSessionState(uint64_t peer, SessionState *out) {
        std::lock_guard<std::mutex> g(mu_);
        if (!out) return false;
        *out = SessionState{};
        if (!accepted_.count(peer) && !seen_.count(peer)) return false;
        out->active = accepted_.count(peer) && seen_.count(peer);
        out->connecting = accepted_.count(peer) && !seen_.count(peer);
        out->remoteIp = INADDR_LOOPBACK;
        out->remotePort = cfg_.peerPort;
        return true;
    }

    std::vector<uint64_t> P2PCore::TakeSessionRequests() {
        std::lock_guard<std::mutex> g(mu_);
        std::vector<uint64_t> out;
        out.swap(pendingRequests_);
        return out;
    }

    bool P2PCore::WaitForPacket(int timeoutMs) {
        std::unique_lock<std::mutex> l(mu_);
        return cv_.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return !queue_.empty() || !pendingRequests_.empty(); });
    }

    Stats P2PCore::GetStats() {
        std::lock_guard<std::mutex> g(mu_);
        return stats_;
    }

    bool ParseConfig(const std::string &text, P2PConfig *cfg, uint64_t *fakeSelfId, std::string *error) {
        std::istringstream in(text);
        std::string line;
        int lineNo = 0;
        bool haveListen = false, havePeer = false;
        *fakeSelfId = 0;
        while (std::getline(in, line)) {
            lineNo++;
            line = Trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                *error = "line " + std::to_string(lineNo) + ": expected key=value";
                return false;
            }
            const auto key = Trim(line.substr(0, eq));
            const auto val = Trim(line.substr(eq + 1));
            uint64_t v = 0;
            if (key == "listen_port" || key == "peer_port") {
                if (!ParseU64(val, &v) || v == 0 || v > 65535) {
                    *error = "line " + std::to_string(lineNo) + ": bad port " + val;
                    return false;
                }
                if (key == "listen_port") {
                    cfg->listenPort = (uint16_t)v;
                    haveListen = true;
                } else {
                    cfg->peerPort = (uint16_t)v;
                    havePeer = true;
                }
            } else if (key == "fake_steam_id") {
                if (val.empty()) continue;
                // individual account in the public universe: 0x0110000100000000 .. +0xFFFFFFFF
                if (!ParseU64(val, &v) || v < 0x0110000100000000ULL || v > 0x01100001FFFFFFFFULL) {
                    *error = "line " + std::to_string(lineNo) + ": fake_steam_id must be an individual SteamID64";
                    return false;
                }
                *fakeSelfId = v;
            }
        }
        if (!haveListen || !havePeer) {
            *error = "listen_port and peer_port are required";
            return false;
        }
        if (cfg->listenPort == cfg->peerPort) {
            *error = "listen_port and peer_port must differ";
            return false;
        }
        cfg->selfId = *fakeSelfId;
        return true;
    }

} // namespace steamproxy
