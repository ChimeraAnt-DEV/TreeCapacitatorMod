#pragma once

#include "tree_capacitor.h"

// Draws a small on-screen HUD showing capacitor network stats via the
// LeviLauncher Mod Menu overlay API (pl::modmenu::submitDrawCommands).
// Rendering is subscription-based: once armed with the trigger key the module
// re-arms itself every time a change happens; the actual draw commands are
// pushed from the simulation thread.

namespace hud {

// Notifies the HUD that network state changed so it re-publishes.
void MarkDirty();

// Publishes the current stats as HUD draw commands.
void RenderStats(const treecap::CapacitorStats& stats);

} // namespace hud