#include "weather_hooks.h"

#include "tree_capacitor.h"

#include <pl/memory/Hook.hpp>
#include <pl/memory/Signature.hpp>
#include <pl/Export.hpp>

#include <android/log.h>

#include <cstdint>

#define LOG_TAG "TreeCap"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace weatherhooks {
namespace {

// Community-verified byte-signature strings (needle format consumed by
// pl::memory::resolveSignature). These match the Weather tick / rain-level
// functions of recent Minecraft Bedrock builds used by LeviLauncher.
//
// Maintenance note: if a Beta/Preview build shifts these functions, update the
// strings below (or extend the list) and re-export the levipack. Until then
// the mod runs in "simulator" mode harmlessly.
constexpr const char* kWeatherTickSignature =
    "? ? ? A9 ? ? ? A9 FD 03 00 91 F3 03 00 AA ? ? ? F9 ? ? ? 39 ? ? ? 34";

constexpr const char* kIsRainingSignature =
    "? ? ? 52 ? ? ? F9 00 01 22 1E ? ? ? 39 ? ? ? 34 ? ? ? 2D 42 38 21 "
    "1E 40 08 20 1E 20 28 20 1E ? ? ? 52 ? ? ? 72 01 01 27 1E 10 20 21 1E";

constexpr const char* kModuleName = "libminecraftpe.so";

// ───────────────────────────────────────────────────────────────────────────
// Detour state
// ───────────────────────────────────────────────────────────────────────────
pl::memory::HookHandle g_weatherTickHandle;
pl::memory::HookHandle g_isRainingHandle;

void (*g_origWeatherTick)(void*) = nullptr;
bool (*g_origIsRaining)(const void*) = nullptr;

bool g_isRaining = false;

// The Weather::tick hook is the most reliable single point to observe state
// changes; when it fires we simply clamp our simulated weather to "rain" or
// "clear" and keep the engine moving. The `isRaining` predicate (when found)
// gives us the authoritative answer cheaply.
void WeatherTickDetour(void* self) {
    if (g_origIsRaining) {
        g_isRaining = g_origIsRaining(self);
    }
    treecap::api::SetWeather(g_isRaining ? treecap::WeatherState::Rain
                                         : treecap::WeatherState::Clear,
                             true);
    if (g_origWeatherTick) {
        g_origWeatherTick(self);
    }
}

bool IsRainingDetour(const void* self) {
    bool raining = false;
    if (g_origIsRaining) {
        raining = g_origIsRaining(self);
    }
    g_isRaining = raining;
    treecap::api::SetWeather(raining ? treecap::WeatherState::Rain
                                     : treecap::WeatherState::Clear,
                             true);
    return raining;
}

} // namespace

bool Install() {
    if (g_weatherTickHandle.installed() || g_isRainingHandle.installed()) {
        return true; // already installed
    }

    // First try the cheapest signal: an isRaining predicate.
    uintptr_t isRainingAddr =
        pl::memory::resolveSignature(kIsRainingSignature, kModuleName);
    if (isRainingAddr) {
        LOGI("Hooking Weather::isRaining at 0x%lx", isRainingAddr);
        g_isRainingHandle =
            pl::memory::HookHandle(reinterpret_cast<void*>(isRainingAddr),
                                   reinterpret_cast<void*>(&IsRainingDetour),
                                   reinterpret_cast<void**>(&g_origIsRaining));
    }

    // Also hook Weather::tick so charge keeps flowing during storms even if
    // the predicate is unavailable.
    uintptr_t weatherTickAddr =
        pl::memory::resolveSignature(kWeatherTickSignature, kModuleName);
    if (weatherTickAddr) {
        LOGI("Hooking Weather::tick at 0x%lx", weatherTickAddr);
        g_weatherTickHandle =
            pl::memory::HookHandle(reinterpret_cast<void*>(weatherTickAddr),
                                   reinterpret_cast<void*>(&WeatherTickDetour),
                                   reinterpret_cast<void**>(&g_origWeatherTick));
    }

    const bool any = g_weatherTickHandle.installed() ||
                     g_isRainingHandle.installed();
    if (any) {
        LOGI("Weather hooks installed");
    } else {
        LOGI("No matching Weather signatures found for this Minecraft build");
    }
    return any;
}

void Uninstall() {
    g_weatherTickHandle.reset();
    g_isRainingHandle.reset();
    g_origWeatherTick = nullptr;
    g_origIsRaining = nullptr;
    g_isRaining = false;
}

} // namespace weatherhooks