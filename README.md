# TreeCapacitator

A native `.so` mod for [LeviLauncher (LeviLaunchroid)](https://github.com/LiteLDev/LeviLaunchroid)
that adds a **tree capacitor network** plus a **real tree feller** to
Minecraft Bedrock Edition.

The mod targets Minecraft Bedrock **1.26 and newer**, and also runs on the
Android LeviLamina API extension
[ApexAntLamina](https://github.com/ChimeraAnt-DEV/ApexAntLamina) — both ship the
same `libpreloader.so` (`pl::`) runtime, which is the only API this mod uses.

## Minecraft version compatibility

The mod is built so that one `.levipack` covers every 1.26+ build instead of
being pinned to a single version:

* `minecraft_versions` in `manifest.json` is left **empty**, which the
  launcher treats as "compatible with every version". An empty list is used
  rather than a list of wildcards because the feller validates its targets at
  runtime and degrades gracefully, so naming a ceiling would only hide the mod
  from builds it actually works on.
* Game functions are resolved in **two tiers**: a version-keyed byte signature
  first (fast on the builds it was recorded from), then an RTTI/vtable lookup
  via `pl::memory::resolveVtableFunction()` (independent of the build number).
* Anything that still cannot be resolved leaves that subsystem dormant without
  affecting the rest of the mod.

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

Yes — the mod is a tree feller. Break the bottom log of any tree with any axe
and the **whole tree** falls: the connected log column, side branches and the
canopy leaves are collected with a flood-fill (capped at 256 blocks by default)
and destroyed through the game's own `GameMode::destroyBlock`, so drops, sound
and block effects behave exactly like a normal break.

Two details worth knowing:

* Only **logs** propagate the search. Leaves are collected but never traversed,
  so two trees whose canopies touch are felled separately. Earlier revisions let
  the flood-fill walk through leaves, which felled a whole grove at once.
* All 1.26 tree species are recognised — oak, spruce, birch, jungle, acacia,
  dark oak, mangrove, cherry, pale oak, bamboo and the nether stems/hyphae — in
  natural, stripped and `wood` forms.

The feller hooks `GameMode::destroyBlock` and reads block names via
`BlockSource::getBlock` + `Block::BlockType::NameInfo`.

The block/name offsets are the one genuinely version-sensitive part, and they
cannot be read off the headers: the ApexAntLamina `Block`/`BlockType` headers are
generated for the Win64 ABI, where `std::string` is 32 bytes, while Android uses
libc++ (24 bytes), so the two disagree and `BlockComponentStorage` has already
grown once inside the 1.26 line. Instead of pinning one tuple, `FullNameOf()`
probes the short list in `kNameLayouts` (in `src/tree_feller.cpp`) and accepts
the first layout that yields a plausible `[a-z0-9_:]+` block name. A wrong
offset effectively never produces that shape, so this survives a layout change
and fails safe — if no layout matches, no name is reported and the feller
declines to cut rather than destroying the wrong blocks.

If nothing can be resolved, the hooks silently don't install and the capacitor
part keeps working.

## Tests

CI compiles the production `tree_feller.cpp` against host stubs and runs a
simulation of the felling flow:

```bash
g++ -std=gnu++20 -O1 -DFELLER_TEST_HOOKS -Isrc -Itests/stubs \
    tests/tree_feller_sim.cpp src/tree_feller.cpp \
    tests/stubs/pl/memory/Hook.cpp tests/stubs/pl/memory/Vtable.cpp \
    tests/stubs/log_impl.cpp \
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