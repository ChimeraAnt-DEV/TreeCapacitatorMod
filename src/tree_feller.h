#pragma once

// Tree Feller — real in-game tree chopping for LeviLaunchroid / MC 1.26.33.1.
//
// When you break the lowest log block of a standing tree (any axe), instead of
// only that block the whole log column plus the canopies attached to it fall.
//
// Implementation approach (fully compatible with LeviLaunchroid's pl:: ABI):
//   * resolve GameMode::destroyBlock + BlockSource::getBlock via the same
//     community-verified byte signatures used by BedrockTools;
//   * hook GameMode::destroyBlock so every block that actually gets destroyed
//     is chased:
//       1. if the destroyed block is a log (log / stripped_log / wood / stem),
//          walk the log column upward, also collecting any canopy located
//          directly above the topmost log — so the tree falls entirely;
//       2. re-destroy every discovered block through the game's own
//          destroyBlock so drops, block entities and sounds behave exactly
//          like the vanilla break;
//   * if a signature is missing on some build, the module stays dormant and
//     the tree capacitor still works on its simulation side.

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
} // namespace testhooks
#endif

} // namespace feller