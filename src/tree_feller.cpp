#include "tree_feller.h"

#include "tree_capacitor.h"

#include <pl/memory/Hook.hpp>
#include <pl/memory/Signature.hpp>
#include <pl/Export.hpp>

#include <android/log.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define LOG_TAG "TreeCap"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace feller {
namespace {

// ───────────────────────────────────────────────────────────────────────────
// Community-verified byte signatures for Minecraft Bedrock (arm64-v8a).
// Same needle format as LeviLaunchroid's own signature rules and BedrockTools.
// ───────────────────────────────────────────────────────────────────────────
constexpr const char* kModuleName = "libminecraftpe.so";

// bool GameMode::destroyBlock(class BlockSource&, BlockPos const&, uint8_t face)
constexpr const char* kDestroyBlockSignature =
    "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? F9 ? ? ? B9 ? ? ? D5"
    " ? ? ? F9 ? ? ? F9 ? ? ? F9 ? ? ? 32 ? ? ? F9 ? ? ? F9 ? ? ? F9";

// const Block* BlockSource::getBlock(BlockPos const&, uint32_t) const
constexpr const char* kBlockSourceGetBlockSignature =
    "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 56 D0 3B D5 ? ? ? F9"
    " ? ? ? F9 ? ? ? B9 ? ? ? 79 1F 01 09 6B ? ? ? 54 ? ? ? 79 F3 03 00 AA"
    " 1F 01 09 6B ? ? ? 54 E0 03 00 91";

// ───────────────────────────────────────────────────────────────────────────
// Game object ABI (pointer-only local reads; offsets match MC arm64 builds).
// ───────────────────────────────────────────────────────────────────────────
using DestroyBlockFn = bool (*)(void* self, void* region, const void* pos,
                                uint8_t face);
using GetBlockFn = const void* (*)(const void* region, const void* pos,
                                   uint32_t data);

DestroyBlockFn g_destroyBlock = nullptr;
GetBlockFn g_getBlock = nullptr;
void* g_destroyBlockAddr = nullptr;

std::atomic<bool> g_enabled{true};
std::atomic<int> g_maxLogs{256};

// ───────────────────────────────────────────────────────────────────────────
// Position
// ───────────────────────────────────────────────────────────────────────────
struct Vec3i {
    int x, y, z;
};

// A "BlockPos" in modern Bedrock is 3 ints (12 bytes) at base + 0x0.
// Keep our ABI assumptions minimal: we always pass a freshly-built 12-byte
// block; region/vtables are untouched.
const void* GetBlockAt(const void* region, const Vec3i& pos) {
    if (!g_getBlock) return nullptr;
    int32_t p[3] = {pos.x, pos.y, pos.z};
    return g_getBlock(region, p, 0);
}

Vec3i DecodePos(const void* posPtr) {
    const auto* p = static_cast<const int8_t*>(posPtr);
    Vec3i out;
    std::memcpy(&out.x, p + 0, sizeof(out.x));
    std::memcpy(&out.y, p + 4, sizeof(out.y));
    std::memcpy(&out.z, p + 8, sizeof(out.z));
    return out;
}

// ───────────────────────────────────────────────────────────────────────────
// Block identification via Block::fullName() (same layout as BedrockTools).
// ───────────────────────────────────────────────────────────────────────────
constexpr std::size_t kBlockMBlockType = 0x68;    // Block::mBlockType
constexpr std::size_t kBlockTypeMNameInfo = 0x88; // BlockType::mNameInfo
constexpr std::size_t kNameInfoMFullName = 0x40;  // NameInfo::mFullName
constexpr std::size_t kHashedStringMString = 0x8; // HashedString::mString

std::string_view FullNameOf(const void* block) {
    if (!block) return {};
    const auto type = *reinterpret_cast<const uintptr_t*>(
        static_cast<const char*>(block) + kBlockMBlockType);
    if (!type) return {};
    const auto hashed = type + kBlockTypeMNameInfo + kNameInfoMFullName;
    const auto* str =
        reinterpret_cast<const std::string*>(hashed + kHashedStringMString);
    if (!str) return {};
    if (str->size() > 512) return {};
    return {str->data(), str->size()};
}

bool IsLogBlock(std::string_view name) {
    constexpr std::string_view prefix = "minecraft:";
    if (name.starts_with(prefix)) name.remove_prefix(prefix.size());
    return name == "log" || name == "log2" || name == "stripped_log" ||
           name == "stripped_log2" || name == "wood" || name == "stripped_wood" ||
           name == "bamboo" || name == "mangrove_log" ||
           name.starts_with("log_") || name.starts_with("stripped_") ||
           name.ends_with("_log") || name.ends_with("_wood") ||
           name == "cherry_log" || name == "cherry_wood" ||
           name == "stripped_cherry_log" || name == "stripped_cherry_wood";
}

// A block counts as part of the tree structure if it is a log or a canopy-ish
// block (leaves, roots). Bamboo is a log; leaves carry the "leave" substring.
bool IsTreeBlock(std::string_view name) {
    if (IsLogBlock(name)) return true;
    if (name.find("leave") != std::string_view::npos) return true;
    if (name.ends_with("_roots")) return true; // mangrove prop roots
    return false;
}

// ───────────────────────────────────────────────────────────────────────────
// Collected blocks: once a real destroy happens and the tree i.d. is done we
// re-destroy the remaining blocks through the original GameMode::destroyBlock.
// The original pointer is called in a dedicated re-entrancy slot so we never
// call it through our own detour.
// ───────────────────────────────────────────────────────────────────────────
void OriginalDestroyBlock(void* self, void* region, const Vec3i& pos,
                          uint8_t face = 0) {
    if (!g_destroyBlock) return;
    int32_t p[3] = {pos.x, pos.y, pos.z};
    (void)g_destroyBlock(self, region, p, face);
}

bool g_inProgress = false; // Serialize the whole felling procedure on one break.

// Pack a position into a 64-bit visit key so negative coordinates are safe:
//   key = ((y+512)&0xFFFF)<<32 | (x&0xFFFF)<<16 | (z&0xFFFF)
int64_t PackKey(const Vec3i& p) {
    const int64_t x = static_cast<int64_t>(p.x) & 0xFFFF;
    const int64_t y = static_cast<int64_t>(p.y) & 0xFFFF;
    const int64_t z = static_cast<int64_t>(p.z) & 0xFFFF;
    return (y << 40) | (x << 20) | z;
}

const std::vector<Vec3i>* CollectTreeBlocks(void* region, const Vec3i& start) {
    // Flood-fill the connected tree component around `start`: logs, leaves and
    // prop roots that touch orthogonally/diagonally, capped at g_maxLogs so a
    // short-circuit (e.g. a player-built log pillar) cannot nuke a 1000-block
    // structure.
    static thread_local std::vector<Vec3i> out;
    static thread_local std::vector<Vec3i> queue;
    out.clear();
    queue.clear();

    // maxNodes is the number of *additional* blocks to collect beyond the broken
// base block (which is already gone when CollectTreeBlocks runs).
int maxNodes = g_maxLogs.load(std::memory_order_relaxed);
    if (maxNodes <= 0) return nullptr;

    out.push_back(start);
    queue.push_back(start);

    // Canonical visit key = packed 32-bit (includes sign bit workaround).
    static thread_local std::vector<int64_t> visited;
    visited.clear();
    visited.push_back(PackKey(start));

    while (!queue.empty()) {
        const Vec3i cur = queue.back();
        queue.pop_back();
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dz = -1; dz <= 1; ++dz) {
                    if (dx == 0 && dy == 0 && dz == 0) continue;
                    const Vec3i nb{cur.x + dx, cur.y + dy, cur.z + dz};
                    const int64_t key = PackKey(nb);
                    // visited is small; linear scan is fine for tree sizes.
                    bool seen = false;
                    for (const int64_t k : visited) {
                        if (k == key) { seen = true; break; }
                    }
                    if (seen) continue;
                    visited.push_back(key);

                    if (static_cast<int>(out.size()) - 1 >= maxNodes) {
                        return &out; // caps the extra blocks
                    }
                    if (IsTreeBlock(FullNameOf(GetBlockAt(region, nb)))) {
                        out.push_back(nb);
                        queue.push_back(nb);
                    }
                    if (static_cast<int>(out.size()) - 1 >= maxNodes) {
                        return &out; // also cap right after growing
                    }
                }
            }
        }
    }
    return &out;
}

bool DestroyBlockDetour(void* self, void* region, const void* posPtr,
                        uint8_t face) {
    // Serialize against re-entrancy (the felled destroys must not re-trigger
    // the same flow forever).
    if (g_inProgress) {
        return g_destroyBlock ? g_destroyBlock(self, region, posPtr, face)
                              : false;
    }

    // Decode early so we can decide BEFORE the block is gone.
    const Vec3i broken = DecodePos(posPtr);

    // Only start a felling run from a log block we actually hit.
    bool isTrunk = false;
    if (g_enabled.load(std::memory_order_relaxed)) {
        const std::string_view hitName = FullNameOf(GetBlockAt(region, broken));
        isTrunk = IsLogBlock(hitName);
    }

    // First: invoke the original — the block breaks vertex-and-drops exactly
    // like vanilla.
    const bool result =
        g_destroyBlock ? g_destroyBlock(self, region, posPtr, face) : false;
    if (!result || !isTrunk) return result;

    g_inProgress = true;
    const std::vector<Vec3i>* tree = CollectTreeBlocks(region, broken);
    if (tree && !tree->empty()) {
        static thread_local int fell = 0;
        int count = 0;
        for (const Vec3i& p : *tree) {
            if (p.y == broken.y && p.x == broken.x && p.z == broken.z) {
                continue; // Already destroyed by the original call.
            }
            if (count >= g_maxLogs.load(std::memory_order_relaxed)) break;
            OriginalDestroyBlock(self, region, p);
            ++count;
        }
        ++fell;
        if (fell % 20 == 0) {
            LOGI("[Feller] felled tree @ %d,%d,%d blocks=%zu",
                 broken.x, broken.y, broken.z, tree->size());
        }
    }

    g_inProgress = false;
    return result;
}

} // namespace

bool Install() {
    if (g_destroyBlockAddr) return true; // already installed

    uintptr_t destroyAddr =
        pl::memory::resolveSignature(kDestroyBlockSignature, kModuleName);
    if (!destroyAddr) {
        LOGI("TreeFeller: GameMode::destroyBlock signature not found — "
             "feller disabled on this build");
        return false;
    }
    g_destroyBlockAddr = reinterpret_cast<void*>(destroyAddr);

    // Resolve the block read primitive used for log detection.
    uintptr_t getBlockAddr =
        pl::memory::resolveSignature(kBlockSourceGetBlockSignature, kModuleName);
    if (getBlockAddr) {
        g_getBlock = reinterpret_cast<GetBlockFn>(getBlockAddr);
    } else {
        LOGI("TreeFeller: BlockSource::getBlock not found — will fall back "
             "to log detection from the broken block only");
    }

    g_destroyBlock = reinterpret_cast<DestroyBlockFn>(g_destroyBlockAddr);
    if (pl::memory::hook(g_destroyBlockAddr,
                         reinterpret_cast<void*>(&DestroyBlockDetour),
                         reinterpret_cast<void**>(&g_destroyBlock),
                         pl::memory::HookPriority::High) != 0) {
        LOGE("TreeFeller: hook() failed");
        g_destroyBlock = reinterpret_cast<DestroyBlockFn>(g_destroyBlockAddr);
        return false;
    }

    LOGI("TreeFeller: hooked GameMode::destroyBlock @ 0x%lx (getBlock=%s)",
         destroyAddr, g_getBlock ? "ok" : "missing");
    return true;
}

void Uninstall() {
    if (g_destroyBlockAddr) {
        pl::memory::unhook(g_destroyBlockAddr,
                           reinterpret_cast<void*>(&DestroyBlockDetour));
        g_destroyBlockAddr = nullptr;
        g_destroyBlock = nullptr;
    }
    g_getBlock = nullptr;
}

void SetEnabled(bool on) { g_enabled.store(on); }
bool IsEnabled() { return g_enabled.load(); }
void SetMaxLogs(int count) { g_maxLogs.store(count > 0 ? count : 1); }
int MaxLogs() { return g_maxLogs.load(); }

#ifdef FELLER_TEST_HOOKS
// Host-side test hooks: let tests inject the fake game primitives and call the
// real detour directly. Compiled out of the shipped mod.
namespace testhooks {
void SetGetBlockPtr(GetBlockFn fn) { g_getBlock = fn; }
void SetDestroyBlockPtr(DestroyBlockFn fn) { g_destroyBlock = fn; }
bool CallDestroyDetour(void* self, void* region, const void* pos, uint8_t face) {
    return DestroyBlockDetour(self, region, pos, face);
}
GetBlockFn GetBlockPtr() { return g_getBlock; }
} // namespace testhooks
#endif

} // namespace feller