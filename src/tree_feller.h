#pragma once

#include <cstddef>

// Tree Feller — real in-game tree chopping for LeviLaunchroid / MC 1.26+.
//
// When you break the lowest log block of a standing tree (any axe), instead of
// only that block the whole log column plus the canopy attached to it fall.
//
// Implementation approach (fully compatible with LeviLaunchroid's pl:: ABI):
//   * resolve GameMode::destroyBlock + BlockSource::getBlock in two tiers:
//       tier 1 — version-keyed byte signatures, fastest on known builds;
//       tier 2 — pl::memory::resolveVtableFunction() RTTI lookup, which is
//                independent of the Minecraft build number;
//   * hook GameMode::destroyBlock so every block that actually gets destroyed
//     is chased:
//       1. if the destroyed block is a log (log / stripped_log / wood / stem /
//          hyphae, all tree species), flood-fill the connected logs to find the
//          trunk, then collect the canopy sitting on it — leaves never
//          propagate, so adjacent trees are not felled together;
//       2. re-destroy every discovered block through the game's own
//          destroyBlock so drops, block entities and sounds behave exactly
//          like the vanilla break;
//   * if both tiers miss (an unknown future build whose class layout changed
//     too), the module stays dormant and the capacitor still works.

namespace feller {

// Attempts to install the tree-feller hooks. Safe to call multiple times.
// Returns true when at least the core destroyBlock hook is live.
bool Install();

// Removes every installed tree-feller hook.
void Uninstall();

// Runtime toggles (wired into the Mod Menu together with the capacitor).
void SetEnabled(bool on);
bool IsEnabled();
void SetMaxLogs(int count);
int MaxLogs();

#ifdef FELLER_TEST_HOOKS
// Host-side test hooks (compiled out of the shipped mod). These small
// declarations keep uint32_t/uint8_t visible without pulling <cstdint> here
// again — the host test TU includes it before this header. In the NDK build
// this block is removed entirely.
typedef unsigned int _feller_u32;
typedef unsigned char _feller_u8;
namespace testhooks {
using GetBlockFn = const void* (*)(const void*, const void*, _feller_u32);
using DestroyBlockFn = bool (*)(void*, void*, const void*, _feller_u8);
void SetGetBlockPtr(GetBlockFn fn);
void SetDestroyBlockPtr(DestroyBlockFn fn);
bool CallDestroyDetour(void* self, void* region, const void* pos, _feller_u8 face);
// Exposes the vtable slot constants so a test can assert they still match the
// compiler's own Itanium slot ordering.
std::size_t GameModeDestroyBlockSlot();
std::size_t BlockSourceGetBlockSlot();
} // namespace testhooks
#endif

} // namespace feller