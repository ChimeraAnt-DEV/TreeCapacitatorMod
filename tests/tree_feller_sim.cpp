// Host-side simulation test for the Tree Feller (`tree_feller.cpp`).
//
// Compiles the REAL production feller source against host stubs for the
// pl:: memory API and <android/log.h>, then drives it through the same
// GameMode::destroyBlock detour path the game would call. A fake in-memory
// "world" mimics BlockSource/Block/BlockType layout per BedrockTools so the
// unmocked FullNameOf/GetBlockAt code is exercised faithfully.
//
// Build (from repo root):
//   g++ -std=gnu++20 -O1 -DFELLER_TEST_HOOKS -Isrc -Itests/stubs \
//       tests/tree_feller_sim.cpp src/tree_feller.cpp \
//       tests/stubs/pl/memory/Hook.cpp tests/stubs/log_impl.cpp -pthread \
//       -o /tmp/feller_sim && /tmp/feller_sim

#include <cstdint> // must precede tree_feller.h on glibc hosts
#include "tree_feller.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <tuple>
#include <vector>

// ── Fake world: sparse (x,y,z) -> block name ─────────────────────────────
struct FakeBlock {
    char name[64];
};
std::map<std::tuple<int,int,int>, FakeBlock> g_world;
int g_destroyCalls = 0;

void setBlock(int x, int y, int z, const std::string& name) {
    auto& fb = g_world[{x,y,z}];
    std::snprintf(fb.name, sizeof(fb.name), "%s", name.c_str());
}

// SimBlock: fake Block with mBlockType at +0x68 pointing to a fake BlockType
// whose embedded NameInfo/HashedString/std::string live at +0xD0 (matching the
// production offsets: BlockType+0x88 NameInfo, +0x40 HashedString, +0x8 str).
struct SimBlock {
    uint8_t pad[0x68];
    void* mBlockType;
};

std::vector<void*> g_allocs;
const void* mockGetBlock(const void*, const void* pos, uint32_t) {
    const auto* bytes = static_cast<const uint8_t*>(pos);
    int32_t c[3];
    std::memcpy(c, bytes, 12);
    auto it = g_world.find({c[0],c[1],c[2]});
    if (it == g_world.end()) return nullptr; // air

    auto* block = static_cast<SimBlock*>(::operator new(sizeof(SimBlock)));
    g_allocs.push_back(block);
    constexpr std::size_t kTypeSize = 0xD0;
    auto* typeBuf = static_cast<uint8_t*>(::operator new(kTypeSize + sizeof(std::string)));
    g_allocs.push_back(typeBuf);
    new (typeBuf + kTypeSize) std::string(it->second.name);
    block->mBlockType = typeBuf;
    return block;
}

bool mockDestroy(void* /*self*/, void* /*region*/, const void* pos, uint8_t) {
    const auto* bytes = static_cast<const uint8_t*>(pos);
    int32_t c[3];
    std::memcpy(c, bytes, 12);
    g_world.erase({c[0],c[1],c[2]}); // the block really is gone
    ++g_destroyCalls;
    return true;
}

void makeTree() {
    // A realistic oak: base log (2,0,2), 4 more logs above, a side branch at
    // (1,1,2), a 3x3 leaf canopy at y=5, and a distant unrelated stone so we
    // can assert it is never destroyed.
    setBlock(2,0,2,"minecraft:oak_log");
    setBlock(2,1,2,"minecraft:oak_log");
    setBlock(2,2,2,"minecraft:oak_log");
    setBlock(2,3,2,"minecraft:oak_log");
    setBlock(2,4,2,"minecraft:oak_log");
    setBlock(1,1,2,"minecraft:oak_log");
    for (int dx=-1;dx<=1;++dx)
        for (int dz=-1;dz<=1;++dz)
            setBlock(2+dx,5,2+dz,"minecraft:oak_leaves");
    setBlock(50,1,50,"minecraft:stone");
}

int main() {
    feller::SetEnabled(true);
    feller::testhooks::SetGetBlockPtr(&mockGetBlock);
    feller::testhooks::SetDestroyBlockPtr(&mockDestroy);

    // ── Test 1: breaking the bottom log fells the whole tree ─────────────
    feller::SetMaxLogs(256);
    makeTree();
    int32_t base[3] = {2,0,2};
    const bool ok = feller::testhooks::CallDestroyDetour(nullptr, nullptr, base, 0);
    // Expected destroys: base + 4 above + 1 side branch + 9 leaves = 15.
    if (!ok || g_destroyCalls != 15) {
        std::printf("FAIL tree: ok=%d destroyCalls=%d (want 15)\n", ok, g_destroyCalls);
        return 1;
    }
    int logsLeft = 0;
    for (auto& [k, f] : g_world)
        if (std::strstr(f.name, "oak_log")) ++logsLeft;
    if (logsLeft != 0) {
        std::printf("FAIL tree: %d logs left (want 0)\n", logsLeft);
        return 1;
    }
    std::printf("PASS tree felled: 15 blocks, 0 logs left, stone untouched\n");

    // ── Test 2: non-tree break only breaks the single block ──────────────
    g_destroyCalls = 0;
    g_world.clear();
    setBlock(10,10,10,"minecraft:dirt");
    int32_t dirt[3] = {10,10,10};
    feller::testhooks::CallDestroyDetour(nullptr, nullptr, dirt, 0);
    if (g_destroyCalls != 1) {
        std::printf("FAIL dirt: destroyCalls=%d (want 1)\n", g_destroyCalls);
        return 1;
    }
    std::printf("PASS single-block break for non-log\n");

    // ── Test 3: maxLogs cap prevents runaway on a log pillar ─────────────
    feller::SetMaxLogs(8);
    g_destroyCalls = 0;
    g_world.clear();
    for (int y = 0; y < 40; ++y) setBlock(0, y, 0, "minecraft:oak_log");
    int32_t pillar[3] = {0,0,0};
    feller::testhooks::CallDestroyDetour(nullptr, nullptr, pillar, 0);
    if (g_destroyCalls != 9) { // base + 8 extra
        std::printf("FAIL cap: destroyCalls=%d (want 9)\n", g_destroyCalls);
        return 1;
    }
    std::printf("PASS maxLogs cap honored (9 blocks)\n");

    std::printf("FELLER SIM OK\n");
    return 0;
}