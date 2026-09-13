#pragma once

// Optional integration with the real in-game weather cycle.
//
// When the running Minecraft build matches one of the shipped byte-signatures
// (community-verified for several recent Bedrock releases, see
// weather_hooks.cpp), the mod hooks the Weather tick/rain-level functions and
// feeds the actual rain/thunder state into the capacitor engine. If no
// signature matches, the mod falls back to a built-in storm simulator so the
// network still charges during "storms".

namespace weatherhooks {

// Attempts to install game-weather hooks. Returns true if at least one hook
// was installed. Safe to call multiple times.
bool Install();

// Removes any installed hooks.
void Uninstall();

} // namespace weatherhooks