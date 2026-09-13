# TreeCapacitator

A native `.so` mod for [LeviLauncher (LeviLaunchroid)](https://github.com/LiteLDev/LeviLaunchroid)
that adds a **tree capacitor network** plus a **real tree feller** to
Minecraft Bedrock Edition.

## How it works

Capacitor roots are spread on a virtual grid around the world origin. During
rain and thunderstorms they absorb charge from the sky. Once a root is fully
charged it releases a **growth pulse** toward nearby trees, then starts
reabsorbing for the next storm.

The mod ships as a `.levipack` (a zip containing `manifest.json` + the
`.so`), which is the native-mod format LeviLaunchroid loads.

### Real vs simulated weather

* When the running Minecraft build matches one of the shipped byte-signatures,
  the mod hooks the weather functions (`Weather::tick` / `Weather::isRaining`)
  through the launcher's `pl::memory` API and charges with the *real* in-game
  weather.
* If no signature matches (e.g. a newer Beta/Preview build), the mod falls back
  to a built-in storm simulator so the network keeps working.

This means the mod **loads and runs on any MC version**; game-binary state
(the exact weather signatures) is best-effort and version-specific.

## Features

* Full LeviLauncher lifecycle (`PLGetModRegistration`: load/enable/disable/unload)
* Mod Menu module with live HUD overlay (nodes, charge %, pulses delivered,
  weather source)
* Toggle via keybind (Android key `T`, keycode 48) or the Mod Menu
* Runtime tunables: rain rate, thunder rate, discharge leak, tree feller
  toggle and per-tree block cap

### Tree feller

Break the bottom log of any tree with any axe and the **whole tree** falls:
the connected log column, side branches, canopy leaves and mangrove prop
roots are collected with a flood-fill (capped at 256 blocks by default) and
destroyed through the game's own `GameMode::destroyBlock`, so drops, sound
and block effects behave exactly like a normal break.

The feller hooks `GameMode::destroyBlock` and reads block names via
`BlockSource::getBlock` + `Block::fullName()`, using community-verified
byte signatures (same ones BedrockTools uses). If a future Minecraft build
moves those functions, the hooks silently don't install and the capacitor
part keeps working.

## Tests

CI compiles the production `tree_feller.cpp` against host stubs and runs a
simulation of the felling flow:

```bash
g++ -std=gnu++20 -O1 -DFELLER_TEST_HOOKS -Isrc -Itests/stubs \
    tests/tree_feller_sim.cpp src/tree_feller.cpp \
    tests/stubs/pl/memory/Hook.cpp tests/stubs/log_impl.cpp \
    -pthread -o /tmp/feller_sim && /tmp/feller_sim
```

Expect `FELLER SIM OK`.

## Build

Requires the Android NDK (r26b) and CMake + Ninja:

```bash
cmake -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-26 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
python3 scripts/package_levipack.py --library build/libtree_capacitor.so --icon assets/icon.png --output TreeCapacitator.levipack
```

The build produces `libtree_capacitor.so` and `TreeCapacitator.levipack`.

## Install

1. Open LeviLauncher → Mods.
2. Import `TreeCapacitator.levipack`.
3. Enable the mod for your Minecraft version.

## Layout

```
src/Main.cpp              mod lifecycle + preloader registration
src/tree_capacitor.cpp    capacitor simulation engine
src/weather_hooks.cpp     optional real-weather hooks (signature-based)
src/hud.cpp               Mod Menu HUD overlay
preloader_headers/pl      vendored preloader SDK headers (build-time)
preloader_stub/           build-time libpreloader.so stub (never shipped)
scripts/                  levipack packaging + verification
```

## Maintenance: updating weather signatures

The weather hooks in `src/weather_hooks.cpp` contain byte-signature strings
that match specific Minecraft Bedrock builds. When a new game version shifts
these functions, update the strings (or extend the list) and rebuild. Until
then the mod runs in simulated-weather mode without breaking.

## Disclaimer

Unofficial mod. Use at your own risk; not affiliated with Mojang or Microsoft.
The authors are not responsible for bans, damages, or issues arising from use.