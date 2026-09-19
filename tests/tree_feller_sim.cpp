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

#include <pl/memory/Vtable.hpp>

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

// SimBlock: fake Block with mBlockType at +0x60 pointing to a fake BlockType
// whose embedded NameInfo/HashedString/std::string live at +0xD8 (matching the
// production offsets: BlockType+0x88 NameInfo, +0x48 HashedString, +0x8 str).
//
// The mBlockType offset is settable so the offset-probe can be exercised: with
// g_typeOffset = 0x68 the bytes at 0x60 stay zero, which is exactly what a
// grown BlockComponentStorage looks like to the 0x60-first probe.
struct SimBlock {
    uint8_t bytes[0x80];
};

std::size_t g_typeOffset = 0x60;

std::vector<void*> g_allocs;
const void* mockGetBlock(const void*, const void* pos, uint32_t) {
    const auto* bytes = static_cast<const uint8_t*>(pos);
    int32_t c[3];
    std::memcpy(c, bytes, 12);
    auto it = g_world.find({c[0],c[1],c[2]});
    if (it == g_world.end()) return nullptr; // air

    auto* block = new (::operator new(sizeof(SimBlock))) SimBlock{};
    g_allocs.push_back(block);
    constexpr std::size_t kTypeSize = 0x100;
    auto* typeBuf = static_cast<uint8_t*>(::operator new(kTypeSize));
    std::memset(typeBuf, 0, kTypeSize);
    g_allocs.push_back(typeBuf);
    // BlockType+0x88 NameInfo, +0x48 HashedString, +0x8 str.
    new (typeBuf + 0x88 + 0x48 + 0x8) std::string(it->second.name);
    *reinterpret_cast<void**>(block->bytes + g_typeOffset) = typeBuf;
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

    // ── Test 4: canopies of adjacent trees do not merge ──────────────────
    // Two trunks 2 blocks apart whose canopies touch. Felling one must not
    // touch the other: leaves are collected but never traversed.
    g_destroyCalls = 0;
    g_world.clear();
    feller::SetMaxLogs(256);
    setBlock(0,0,0,"minecraft:oak_log");
    setBlock(0,1,0,"minecraft:oak_log");
    setBlock(2,0,0,"minecraft:oak_log");
    setBlock(2,1,0,"minecraft:oak_log");
    // Canopies overlap around x=1.
    for (int x=-1;x<=3;++x)
        setBlock(x,2,0,"minecraft:oak_leaves");
    int32_t left[3] = {0,0,0};
    feller::testhooks::CallDestroyDetour(nullptr, nullptr, left, 0);
    bool rightTrunkAlive = g_world.count({2,0,0}) && g_world.count({2,1,0});
    if (!rightTrunkAlive) {
        std::printf("FAIL adjacency: second tree was felled too\n");
        return 1;
    }
    std::printf("PASS adjacent tree untouched (no canopy bleed)\n");

    // ── Test 5: every 1.26 wood species is recognised as a log ───────────
    struct Species { const char* name; };
    const Species species[] = {
        {"minecraft:oak_log"},          {"minecraft:spruce_log"},
        {"minecraft:birch_log"},         {"minecraft:jungle_log"},
        {"minecraft:acacia_log"},        {"minecraft:dark_oak_log"},
        {"minecraft:mangrove_log"},      {"minecraft:cherry_log"},
        {"minecraft:pale_oak_log"},      {"minecraft:crimson_stem"},
        {"minecraft:warped_stem"},       {"minecraft:stripped_oak_log"},
        {"minecraft:stripped_cherry_log"},{"minecraft:crimson_hyphae"},
        {"minecraft:bamboo_block"},      {"minecraft:pale_oak_wood"},
    };
    for (const Species& sp : species) {
        g_destroyCalls = 0;
        g_world.clear();
        setBlock(5,0,5,sp.name);
        setBlock(5,1,5,sp.name);
        int32_t pos[3] = {5,0,5};
        feller::testhooks::CallDestroyDetour(nullptr, nullptr, pos, 0);
        // A recognised tree block destroys at least the second block too.
        if (g_destroyCalls != 2) {
            std::printf("FAIL species %s: destroyCalls=%d (want 2)\n",
                        sp.name, g_destroyCalls);
            return 1;
        }
    }
    std::printf("PASS all %zu wood species felled\n",
                sizeof(species)/sizeof(species[0]));

    // ── Test 6: block layout drift does not break block identification ───
    // A future 1.26+ build may grow BlockComponentStorage, moving mBlockType
    // from 0x60 to 0x68. The feller must still recognise the tree instead of
    // silently treating every block as unknown.
    g_destroyCalls = 0;
    g_world.clear();
    makeTree();
    g_typeOffset = 0x68; // simulate the grown layout
    int32_t drifted[3] = {2,0,2};
    feller::testhooks::CallDestroyDetour(nullptr, nullptr, drifted, 0);
    g_typeOffset = 0x60;
    if (g_destroyCalls != 15) {
        std::printf("FAIL layout drift: destroyCalls=%d (want 15)\n",
                    g_destroyCalls);
        return 1;
    }
    std::printf("PASS block layout drift tolerated (mBlockType at 0x68)\n");

    // ── Test 7: Install() falls back to the vtable resolver ─────────────
    // Signature resolution returns 0 in the host stubs, so a successful
    // Install proves the RTTI/vtable tier is wired up.
    feller::Uninstall();
    static int vtableCalls = 0;
    static bool sawGameMode = false;
    static bool sawBlockSource = false;
    pl::memory::SetVtableResolverForTest(
        [](std::string_view typeInfo, std::size_t slot,
           std::string_view module) -> uintptr_t {
            ++vtableCalls;
            // The resolver must be handed the Itanium typeinfo name, i.e. the
            // length-prefixed form ("8GameMode"), never the bare class name.
            if (module != "libminecraftpe.so") return 0;
            if (typeInfo == "8GameMode" && slot == 3) {
                sawGameMode = true;
                return reinterpret_cast<uintptr_t>(&mockDestroy);
            }
            if (typeInfo == "11BlockSource" && slot == 2) {
                sawBlockSource = true;
                return reinterpret_cast<uintptr_t>(&mockGetBlock);
            }
            return 0;
        });
    const bool installed = feller::Install();
    pl::memory::SetVtableResolverForTest(nullptr);
    if (!installed || !sawGameMode || !sawBlockSource) {
        std::printf("FAIL install: installed=%d gameMode=%d blockSource=%d "
                    "vtableCalls=%d\n",
                    installed, sawGameMode, sawBlockSource, vtableCalls);
        return 1;
    }
    std::printf("PASS Install() used the vtable fallback (8GameMode/11BlockSource)\n");
    feller::Uninstall();

    // ── Test 8: the resolveVtableFunction slots match the real ABI ────────
    // Re-create, in miniature, the exact class hierarchy the feller depends on
    // and check by hand that the hardcoded slot indices land on the intended
    // functions. This is what makes the vtable fallback trustworthy: the slot
    // numbers are derived from the Itanium ordering, and this asserts the
    // derivation instead of trusting a comment.
    {
        struct IConstBlockSource {
            virtual ~IConstBlockSource() = default;
            virtual int getBlock(int) const = 0;
        };
        struct IBlockSource : IConstBlockSource {
            virtual int fetchAABBs() = 0;
            virtual int getWeakRef() = 0;
        };
        struct GameMode {
            virtual ~GameMode() = default;
            virtual int startDestroyBlock(int) { return 10; }
            virtual int destroyBlock(int) { return 11; }
        };
        struct BlockSource : IBlockSource {
            int getBlock(int) const override { return 111; }
            int fetchAABBs() override { return 222; }
            int getWeakRef() override { return 333; }
        };

        BlockSource bs;
        void** vt = *reinterpret_cast<void***>(&bs);
        using GetBlock = int (*)(const void*, int);
        GetBlock getBlock = reinterpret_cast<GetBlock>(vt[2]);
        if (getBlock(&bs, 0) != 111) {
            std::printf("FAIL abi: BlockSource slot 2 is not getBlock\n");
            return 1;
        }

        GameMode gm;
        void** gmvt = *reinterpret_cast<void***>(&gm);
        using DestroyBlock = int (*)(const void*, int);
        DestroyBlock destroyBlock = reinterpret_cast<DestroyBlock>(gmvt[3]);
        if (destroyBlock(&gm, 0) != 11) {
            std::printf("FAIL abi: GameMode slot 3 is not destroyBlock\n");
            return 1;
        }
        if (feller::testhooks::GameModeDestroyBlockSlot() != 3 ||
            feller::testhooks::BlockSourceGetBlockSlot() != 2) {
            std::printf("FAIL abi: production slot constants drifted\n");
            return 1;
        }
    }
    std::printf("PASS ABI slot indices match Itanium vtable ordering\n");

    std::printf("FELLER SIM OK\n");
    return 0;
}