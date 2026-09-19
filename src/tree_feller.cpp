#include "tree_feller.h"

#include "tree_capacitor.h"

#include <pl/memory/Hook.hpp>
#include <pl/memory/Signature.hpp>
#include <pl/memory/Vtable.hpp>
#include <pl/Export.hpp>

#include <android/log.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <initializer_list>
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
// Hook target resolution.
//
// Bedrock moves these functions between builds, so a single hardcoded byte
// signature only ever works on one Minecraft version. Instead we resolve the
// targets in two tiers, the same way libpreloader.so resolves its own game
// hooks (see its GameHooks.cpp):
//
//   1. version-keyed byte signatures — fast and reliable on the builds listed
//      below. Each entry carries the [min, max] Minecraft version range it was
//      verified for, mirroring LeviLaunchroid's own signature-rules format.
//   2. RTTI/vtable fallback — pl::memory::resolveVtableFunction() walks the
//      libminecraftpe.so RTTI to find the vtable of a named class and reads a
//      function pointer out of a fixed slot. The slot indices below are
//      derived from the Itanium ABI ordering of the class virtuals, so they
//      stay valid as long as the class' virtual surface does not change.
//
// Together these let one mod build install on any 1.26+ version: newer builds
// simply skip tier 1 and land on tier 2. If both fail, the feller stays
// dormant and the capacitor keeps working.
// ───────────────────────────────────────────────────────────────────────────
constexpr const char* kModuleName = "libminecraftpe.so";

struct SignatureRule {
    const char* verifiedFor; // Minecraft versions this signature was seen on
    const char* signature;
};

// bool GameMode::destroyBlock(BlockPos const&, uint8_t face)
constexpr SignatureRule kDestroyBlockRules[] = {
    {"1.26.30+",
     "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? F9 ? ? ? B9 ? ? ? D5 ? ? ? F9"
     " ? ? ? F9 ? ? ? F9 ? ? ? 32 ? ? ? F9 ? ? ? F9 ? ? ? F9"},
    {"1.26.0-1.26.29",
     "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? F9 ? ? ? B9 ? ? ? D5 ? ? ? F9"
     " ? ? ? F9 ? ? ? F9 ? ? ? 72 ? ? ? F9 ? ? ? F9 ? ? ? F9"},
};

// const Block& BlockSource::getBlock(BlockPos const&) const
constexpr SignatureRule kGetBlockRules[] = {
    {"1.26.30+",
     "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 56 D0 3B D5 ? ? ? F9 ? ? ? F9"
     " ? ? ? B9 ? ? ? 79 1F 01 09 6B ? ? ? 54 ? ? ? 79 F3 03 00 AA 1F 01 09 6B"
     " ? ? ? 54 E0 03 00 91"},
    {"1.26.0-1.26.29",
     "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 56 D0 3B D5 ? ? ? F9 ? ? ? F9"
     " ? ? ? B9 ? ? ? 79 1F 01 09 6B ? ? ? 54 ? ? ? 79 F3 03 00 AA 1F 01 09 6B"
     " ? ? ? 54 C0 03 5F D6"},
};

// Itanium primary-vtable slots (0-based from the address point). The virtual
// destructor occupies slots 0 and 1, so the first declared non-destructor
// virtual is slot 2. Verified against ApexAntLamina's game headers and by
// compiling a stand-in with the same hierarchy (see tests).
constexpr std::size_t kGameModeDestroyBlockSlot = 3;  // GameMode: ~dtor(D0/D1), startDestroyBlock, destroyBlock
// getBlock is introduced by the root interface. Overrides keep the slot they
// were first declared in, so it stays at slot 2 in BlockSource's own primary
// vtable; the concrete class is also the only one guaranteed to *have* a
// vtable+RTTI emitted, which is what resolveVtableFunction needs.
constexpr std::size_t kBlockSourceGetBlockSlot = 2;   // BlockSource: ~dtor(D0/D1), getBlock

// Returns the first of these signatures that resolves on the running build, or
// 0. We deliberately do not pre-filter by Minecraft version: a signature that
// matches the loaded libminecraftpe.so is proof the build is one we know, and a
// build we do not know simply falls through to the vtable tier.
template <std::size_t N>
uintptr_t ResolveFirst(const SignatureRule (&rules)[N]) {
    for (const SignatureRule& rule : rules) {
        if (const uintptr_t addr =
                pl::memory::resolveSignature(rule.signature, kModuleName)) {
            return addr;
        }
    }
    return 0;
}

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
// Block identification via Block::mBlockType -> BlockType::mNameInfo (same
// layout as BedrockTools and the ApexAntLamina 1.26 headers).
//
//   Block      +0x000 mComponents (0x60)   +0x060 mBlockType
//   BlockType  +0x000 mDescriptionId        +0x020 mMaterial
//              +0x028 mComponents (0x60)    +0x088 mNameInfo
//   NameInfo   +0x000 mRawName              +0x030 mNamespaceName
//              +0x040 mFullName             ...
//   HashedString +0x000 mStrHash  +0x008 mStr (std::string)
//
// The ApexAntLamina headers annotate these fields as TypedStorage<8, 32,
// std::string>, i.e. they were generated against the Win64/libstdc++ ABI where
// std::string is 32 bytes. Android builds with libc++ (c++_shared), where
// std::string is 24 bytes, so NameInfo's members sit at 0x28/0x40 rather than
// 0x30/0x50 and the derived offsets do not agree between the two ABIs. The
// offsets therefore cannot be settled from headers alone, and they are not
// stable across Bedrock builds anyway (BlockComponentStorage has already grown
// once inside the 1.26 line).
//
// Rather than betting on one tuple, we probe the small set of layouts below
// and accept the first that yields a plausible block name. A wrong offset
// almost never produces a string of the exact "[a-z0-9_:]+" shape a block name
// has, so this both survives a layout change and fails safe: if nothing looks
// like a name we report no name and the feller declines to cut.
// ───────────────────────────────────────────────────────────────────────────
struct NameLayout {
    std::size_t blockType;  // Block::mBlockType
    std::size_t nameInfo;   // BlockType::mNameInfo
    std::size_t fullName;   // NameInfo::mFullName (HashedString)
    std::size_t string;     // HashedString::mStr (std::string)
};

constexpr NameLayout kNameLayouts[] = {
    {0x60, 0x88, 0x48, 0x8}, // 1.26.20+ as measured on-device
    {0x60, 0x88, 0x40, 0x8}, // libc++ 24-byte std::string spacing
    {0x68, 0x88, 0x48, 0x8}, // pre-1.26.20 BlockComponentStorage size
};

// A block name is a lowercase identifier, optionally namespaced. Anything else
// means we read the wrong address.
bool LooksLikeBlockName(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    for (const char ch : name) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                        ch == '_' || ch == ':';
        if (!ok) return false;
    }
    return true;
}

std::string_view FullNameOf(const void* block) {
    if (!block) return {};
    const auto* base = static_cast<const char*>(block);
    for (const NameLayout& layout : kNameLayouts) {
        const auto type = *reinterpret_cast<const uintptr_t*>(
            base + layout.blockType);
        if (!type) continue;

        // Read through the target's own std::string so its internal layout
        // (libc++ vs libstdc++) is always right; only the outer offsets are
        // probed.
        const auto* str = reinterpret_cast<const std::string*>(
            type + layout.nameInfo + layout.fullName + layout.string);
        const std::size_t size = str->size();
        if (size == 0 || size > 64) continue;

        const std::string_view name{str->data(), size};
        if (LooksLikeBlockName(name)) return name;
    }
    return {};
}

std::string_view StripNamespace(std::string_view name) {
    constexpr std::string_view prefix = "minecraft:";
    if (name.starts_with(prefix)) name.remove_prefix(prefix.size());
    return name;
}

bool EqualAny(std::string_view name, std::initializer_list<std::string_view> names) {
    for (const std::string_view candidate : names) {
        if (name == candidate) return true;
    }
    return false;
}

// Wood-bearing blocks that a tree is built from. Covers every tree type added
// through 1.26: oak, spruce, birch, jungle, acacia, dark oak, mangrove, cherry,
// pale oak, bamboo and the nether fungi (stems/hyphae), in natural, stripped,
// wood, log and `_wood`/`_log` variants.
bool IsLogBlock(std::string_view rawName) {
    const std::string_view name = StripNamespace(rawName);
    return EqualAny(name, {
        "log", "log2", "wood", "stripped_log", "stripped_log2",
        "stripped_wood", "stripped_acacia_wood", "crimson_stem",
        "warped_stem", "stripped_crimson_stem", "stripped_warped_stem",
        "crimson_hyphae", "warped_hyphae",
        "stripped_crimson_hyphae", "stripped_warped_hyphae",
        "bamboo_block", "stripped_bamboo_block",
    }) || name.ends_with("_log") || name.ends_with("_wood") ||
        name.ends_with("_stem") || name.ends_with("_hyphae") ||
        name == "bamboo";
}

// Canopy blocks that belong to a tree but must not extend the search: leaves
// and the roots/hangings attached to them. Falling through from one tree's
// canopy into its neighbour's is what made the previous flood-fill chop
// several trees at once.
bool IsCanopyBlock(std::string_view rawName) {
    const std::string_view name = StripNamespace(rawName);
    return name.find("leave") != std::string_view::npos ||
           name.ends_with("_roots") || name == "roots" || name == "mangrove_roots" ||
           name == "bamboo" || name == "weeping_vines" ||
           name == "twisting_vines" || name == "vine";
}

// Anything a felling run should remove, but only logs propagate the search.
bool IsTreeBlock(std::string_view name) {
    return IsLogBlock(name) || IsCanopyBlock(name);
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

// Collects the tree attached to `start`: the connected log column (which
// propagates the search) plus the canopy sitting on top of it. Leaves are
// collected but never traversed, so two trees whose canopies touch no longer
// get felled together; the canopy scan is bounded to the trunk's own column.
//
// Returns a thread-local list whose first element is `start` (already destroyed
// by the caller) followed by the blocks still to fell. Never more than
// g_maxLogs entries beyond `start`.
const std::vector<Vec3i>* CollectTreeBlocks(void* region, const Vec3i& start) {
    static thread_local std::vector<Vec3i> out;
    static thread_local std::vector<Vec3i> queue;
    out.clear();
    queue.clear();

    // maxNodes is the number of *additional* blocks to collect beyond the broken
    // base block (which is already gone when CollectTreeBlocks runs).
    const int maxNodes = g_maxLogs.load(std::memory_order_relaxed);
    if (maxNodes <= 0) return nullptr;

    out.push_back(start);

    // ── Phase 1: connected logs above/beside the broken block ────────────
    // Only logs join the queue, so a canopy can never bridge two trees.
    queue.push_back(start);
    int trunkMinX = start.x, trunkMaxX = start.x;
    int trunkMinZ = start.z, trunkMaxZ = start.z;
    int trunkMaxY = start.y;

    // Bounded set of visited positions; linear scan is fine for tree sizes.
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
                    bool seen = false;
                    for (const int64_t k : visited) {
                        if (k == key) { seen = true; break; }
                    }
                    if (seen) continue;
                    visited.push_back(key);

                    if (!IsLogBlock(FullNameOf(GetBlockAt(region, nb)))) {
                        continue; // leaves and everything else do not propagate
                    }
                    if (static_cast<int>(out.size()) - 1 >= maxNodes) {
                        return &out; // cap reached; stop growing the tree
                    }
                    out.push_back(nb);
                    queue.push_back(nb);
                    trunkMinX = std::min(trunkMinX, nb.x);
                    trunkMaxX = std::max(trunkMaxX, nb.x);
                    trunkMinZ = std::min(trunkMinZ, nb.z);
                    trunkMaxZ = std::max(trunkMaxZ, nb.z);
                    trunkMaxY = std::max(trunkMaxY, nb.y);
                }
            }
        }
    }

    // ── Phase 2: canopy sitting directly on this trunk ───────────────────
    // Also covers the leaves adjacent to the trunk even when a custom tree has
    // no leaves stacked right on top. Bounded to a 2-block skirt around the
    // trunk's x/z extent so a neighbouring tree's canopy is left alone.
    //
    // Note phase 1's `visited` also holds non-log cells it examined, so it
    // cannot be used here; membership in `out` is what identifies a log we
    // have already queued for felling.
    const int skirt = 2;
    auto alreadyQueued = [&](const Vec3i& p) {
        const int64_t key = PackKey(p);
        for (const Vec3i& q : out) {
            if (PackKey(q) == key) return true;
        }
        return false;
    };
    for (int x = trunkMinX - skirt; x <= trunkMaxX + skirt; ++x) {
        for (int z = trunkMinZ - skirt; z <= trunkMaxZ + skirt; ++z) {
            for (int y = trunkMaxY - 1; y <= trunkMaxY + 3; ++y) {
                const Vec3i pos{x, y, z};
                if (alreadyQueued(pos)) continue;
                if (static_cast<int>(out.size()) - 1 >= maxNodes) {
                    return &out; // cap also applies to canopy blocks
                }
                const std::string_view name = FullNameOf(GetBlockAt(region, pos));
                if (IsCanopyBlock(name)) {
                    out.push_back(pos);
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

    // Tier 1: try every version-keyed signature. We cannot always know the
    // running build from here (the preloader only hands it to its own rules
    // loader), but a signature either matches this build or it does not, so
    // trying them in turn is both safe and version-independent.
    uintptr_t destroyAddr = ResolveFirst(kDestroyBlockRules);

    // Tier 2: RTTI/vtable lookup. Unlike a byte signature, a vtable slot is
    // valid on any build whose class virtual order is unchanged, so this is
    // what makes newer 1.26+ builds work without a new signature. The name is
    // the Itanium ABI "typeinfo name": the mangled form is the length-prefixed
    // "_ZTS8GameMode", and the stored string the preloader searches .rodata for
    // is "8GameMode" — not "9GameMode".
    bool viaVtable = false;
    if (!destroyAddr) {
        destroyAddr = pl::memory::resolveVtableFunction(
            "8GameMode", kGameModeDestroyBlockSlot, kModuleName);
        viaVtable = destroyAddr != 0;
    }
    if (!destroyAddr) {
        LOGI("TreeFeller: GameMode::destroyBlock not found (signature + "
             "vtable) — feller disabled on this build");
        return false;
    }
    g_destroyBlockAddr = reinterpret_cast<void*>(destroyAddr);

    // Resolve the block read primitive used for log detection.
    uintptr_t getBlockAddr = ResolveFirst(kGetBlockRules);
    if (!getBlockAddr) {
        getBlockAddr = pl::memory::resolveVtableFunction(
            "11BlockSource", kBlockSourceGetBlockSlot, kModuleName);
    }
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

    LOGI("TreeFeller: hooked GameMode::destroyBlock @ 0x%lx via %s "
         "(getBlock=%s)",
         destroyAddr, viaVtable ? "vtable" : "signature",
         g_getBlock ? "ok" : "missing");
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
std::size_t GameModeDestroyBlockSlot() { return kGameModeDestroyBlockSlot; }
std::size_t BlockSourceGetBlockSlot() { return kBlockSourceGetBlockSlot; }
} // namespace testhooks
#endif

} // namespace feller