// Host test for src/MemScanCore.h: a fake object graph in this process, read through a guarded reader.
#include "../src/MemScanCore.h"

#include <cstdio>
#include <cstdlib>
#include <map>

using namespace Dynamite::QA;

static std::map<uintptr_t, size_t> g_blocks; // readable blocks; everything else is "unmapped"
static int g_fail = 0;

static size_t Read(uintptr_t addr, void *out, size_t n) {
    auto it = g_blocks.upper_bound(addr);
    if (it == g_blocks.begin()) return 0;
    --it;
    if (addr >= it->first + it->second) return 0;
    const size_t can = it->first + it->second - addr;
    const size_t got = n < can ? n : can;
    std::memcpy(out, reinterpret_cast<void *>(addr), got);
    return got;
}

static uint8_t *Block(size_t n) {
    auto *p = static_cast<uint8_t *>(std::aligned_alloc(16, n));
    std::memset(p, 0, n);
    g_blocks[reinterpret_cast<uintptr_t>(p)] = n;
    return p;
}
static void PutPtr(uint8_t *b, size_t off, const void *p) { std::memcpy(b + off, &p, 8); }
static void PutVec(uint8_t *b, size_t off, float x, float y, float z) {
    const float v[3] = {x, y, z};
    std::memcpy(b + off, v, 12);
}
static void Check(bool ok, const char *what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_fail++;
}

int main() {
    uint8_t *root = Block(0x100), *a = Block(0x80), *b = Block(0x400), *far = Block(0x40), *loop = Block(0x40);
    PutPtr(root, 0x40, a);
    PutPtr(a, 0x10, b);
    PutVec(b, 0x20 + 0x10 * 3, 100.5f, 20.25f, -300.0f); // 4th entry of a 16-byte position table
    PutPtr(root, 0x48, far);
    PutVec(far, 0x8, 5000.0f, 20.0f, -300.0f); // not near the target
    PutVec(far, 0x18, 100.0f, 20.0f, 5000.0f);  // right x and y, wrong z
    PutVec(far, 0x28, 100.0f, 900.0f, -300.0f); // right x and z, wrong y
    PutPtr(root, 0x50, loop);
    PutPtr(loop, 0x0, root);                                         // cycle back to the root
    PutPtr(root, 0x58, reinterpret_cast<void *>(uintptr_t(0x7000000000))); // unmapped pointer
    PutPtr(root, 0x60, reinterpret_cast<void *>(uintptr_t(0x1234567)));    // unaligned: not a pointer

    ScanOptions o;
    o.x = 100;
    o.y = 20;
    o.z = -300;
    o.radius = 2;
    o.window = 0x400;
    const auto rootAddr = reinterpret_cast<uintptr_t>(root);
    std::string res = ScanForVec3(rootAddr, o, Read);
    Check(res.find("40>10@50 100.500 20.250 -300.000\n") != std::string::npos, "finds the vec3 two hops down, with its path");
    Check(res.find("hits=1\n") != std::string::npos, "exactly one hit (the far vec3 and the cycle add none)");
    Check(res.find("truncated") == std::string::npos, "not truncated");

    float v[3] = {0, 0, 0};
    Check(ReadVec3ByPath(rootAddr, "40>10@50", Read, v) && v[0] == 100.5f && v[1] == 20.25f && v[2] == -300.0f, "path reads the same vec3 back");
    PutVec(b, 0x50, 101.0f, 20.25f, -299.0f);
    Check(ReadVec3ByPath(rootAddr, "40>10@50", Read, v) && v[0] == 101.0f && v[2] == -299.0f, "path follows the value when it moves");
    Check(!ReadVec3ByPath(rootAddr, "58@0", Read, v), "unmapped hop -> false, no crash");
    Check(!ReadVec3ByPath(rootAddr, "40>zz@0", Read, v) && !ReadVec3ByPath(rootAddr, "40>10", Read, v), "bad path syntax -> false");
    PutVec(root, 0x80, 100.0f, 21.0f, -301.0f);
    Check(ReadVec3ByPath(rootAddr, "@80", Read, v) && v[1] == 21.0f, "vec3 inside the root itself");

    o.depth = 1;
    res = ScanForVec3(rootAddr, o, Read);
    Check(res.find("40>10@50") == std::string::npos && res.find("@80 ") != std::string::npos, "depth limit is respected");
    o.depth = 4;
    o.skipLo = reinterpret_cast<uintptr_t>(a);
    o.skipHi = o.skipLo + 0x80;
    Check(ScanForVec3(rootAddr, o, Read).find("40>10@50") == std::string::npos, "pointers into the skip range are not followed");
    o.skipLo = o.skipHi = 0;
    o.maxNodes = 1;
    Check(ScanForVec3(rootAddr, o, Read).find("truncated") != std::string::npos, "node limit reports truncation");
    Check(ScanForVec3(0, o, Read) == "nodes=0 hits=0\n", "null root");

    for (auto &kv : g_blocks) std::free(reinterpret_cast<void *>(kv.first));
    std::printf("%s\n", g_fail == 0 ? "OK" : "FAILED");
    return g_fail == 0 ? 0 : 1;
}
