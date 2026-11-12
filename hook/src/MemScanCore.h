#ifndef HOOK_MEMSCANCORE_H
#define HOOK_MEMSCANCORE_H

// Test-only (QA bridge): finds where a game object keeps a position, without knowing its layout.
//
// The game tells Lua the position of soldiers but not of the horse or vehicles. A test can still put
// such an object at a known place (e.g. call D-Horse to a point), then ask: starting at the object's
// pointer and following pointers a few levels deep, where is a float triple near that point? The
// answer is a path of offsets ("40>10>18@20": deref +0x40, deref +0x10, deref +0x18, vec3 at +0x20)
// that ReadVec3ByPath can read again every frame.
//
// Portable and header-only: all memory access goes through ReadFn, so it is tested off-game.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

namespace Dynamite::QA {
    // Copies up to n bytes from addr; returns how many were readable (0 = not readable).
    using ReadFn = size_t (*)(uintptr_t addr, void *out, size_t n);

    struct ScanOptions {
        float x = 0, y = 0, z = 0;
        float radius = 5;
        int depth = 4;          // pointer hops from the root
        size_t window = 0x800;  // bytes examined at each node
        size_t maxNodes = 20000;
        size_t maxHits = 200;
        uintptr_t skipLo = 0, skipHi = 0; // pointers into this range are not followed (the exe image)
    };

    inline bool LooksLikePointer(const uint64_t v, const ScanOptions &o) {
        if (v < 0x10000 || v > 0x00007FFFFFFFFFFFull || (v & 7) != 0) return false;
        if (o.skipHi > o.skipLo && v >= o.skipLo && v < o.skipHi) return false;
        return true;
    }

    // One line per hit: "<path>@<offset> x y z". Last line: "nodes=<n> hits=<n>[ truncated]".
    inline std::string ScanForVec3(const uintptr_t root, const ScanOptions &o, const ReadFn read) {
        struct Node {
            uintptr_t addr;
            std::string path;
            int depth;
        };
        std::string out;
        std::deque<Node> queue;
        std::unordered_set<uintptr_t> seen;
        std::vector<uint8_t> buf(o.window);
        size_t nodes = 0, hits = 0;
        bool truncated = false;
        if (root != 0) {
            queue.push_back({root, "", 0});
            seen.insert(root);
        }
        while (!queue.empty()) {
            if (nodes >= o.maxNodes) {
                truncated = true;
                break;
            }
            const Node node = queue.front();
            queue.pop_front();
            const size_t got = read(node.addr, buf.data(), o.window);
            if (got < 12) continue;
            nodes++;
            for (size_t off = 0; off + 12 <= got; off += 4) {
                float v[3];
                std::memcpy(v, buf.data() + off, 12);
                if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) continue;
                if (std::fabs(v[0] - o.x) > o.radius || std::fabs(v[1] - o.y) > o.radius || std::fabs(v[2] - o.z) > o.radius) continue;
                if (hits >= o.maxHits) {
                    truncated = true;
                    break;
                }
                hits++;
                char line[160];
                std::snprintf(line, sizeof(line), "%s@%zx %.3f %.3f %.3f\n", node.path.c_str(), off, v[0], v[1], v[2]);
                out += line;
            }
            if (node.depth >= o.depth) continue;
            for (size_t off = 0; off + 8 <= got; off += 8) {
                uint64_t p;
                std::memcpy(&p, buf.data() + off, 8);
                if (!LooksLikePointer(p, o) || seen.count(p)) continue;
                seen.insert(p);
                char hop[24];
                std::snprintf(hop, sizeof(hop), "%zx", off);
                queue.push_back({static_cast<uintptr_t>(p), node.path.empty() ? hop : node.path + ">" + hop, node.depth + 1});
            }
        }
        char tail[96];
        std::snprintf(tail, sizeof(tail), "nodes=%zu hits=%zu%s\n", nodes, hits, truncated ? " truncated" : "");
        return out + tail;
    }

    // path: "40>10>18@20" or "@20" (vec3 inside the root itself). Offsets are hex.
    inline bool ReadVec3ByPath(uintptr_t addr, const std::string &path, const ReadFn read, float out[3]) {
        const size_t at = path.find('@');
        if (at == std::string::npos || addr == 0) return false;
        size_t i = 0;
        while (i < at) {
            size_t j = path.find('>', i);
            if (j == std::string::npos || j > at) j = at;
            char *end = nullptr;
            const std::string tok = path.substr(i, j - i);
            const unsigned long long off = std::strtoull(tok.c_str(), &end, 16);
            if (tok.empty() || *end != 0) return false;
            uint64_t next = 0;
            if (read(addr + off, &next, 8) != 8 || next < 0x10000) return false;
            addr = static_cast<uintptr_t>(next);
            i = j + 1;
        }
        char *end = nullptr;
        const std::string tok = path.substr(at + 1);
        const unsigned long long off = std::strtoull(tok.c_str(), &end, 16);
        if (tok.empty() || *end != 0) return false;
        return read(addr + off, out, 12) == 12;
    }
} // namespace Dynamite::QA

#endif // HOOK_MEMSCANCORE_H
